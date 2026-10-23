package server

import (
	"context"
	"crypto/rand"
	"database/sql"
	"errors"
	"fmt"
	"math/big"
	"strings"

	_ "modernc.org/sqlite"
)

var cnDigits = []string{"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"}
var cnUnits = []string{"", "十"}

// RoomName renders ids 1..999 as 一号房..九百九十九号房.
func RoomName(id int64) string {
	if id <= 0 {
		return fmt.Sprintf("%d号房", id)
	}
	var b strings.Builder
	hundreds := id / 100
	rest := id % 100
	if hundreds > 0 {
		b.WriteString(cnDigits[hundreds])
		b.WriteString("百")
		if rest > 0 && rest < 10 {
			b.WriteString("零")
		}
	}
	tens := rest / 10
	ones := rest % 10
	if tens > 0 {
		if tens > 1 || hundreds > 0 {
			b.WriteString(cnDigits[tens])
		}
		b.WriteString("十")
	}
	if ones > 0 {
		b.WriteString(cnDigits[ones])
	}
	b.WriteString("号房")
	return b.String()
}

type Device struct {
	ID           string
	Token        string
	Muted        bool
	ResumeRoomID sql.NullInt64
}

type Room struct {
	ID         int64
	Name       string
	InviteCode string
	JanusRoom  int64
}

type Store struct {
	db *sql.DB
}

func OpenStore(path string) (*Store, error) {
	db, err := sql.Open("sqlite", path+"?_pragma=journal_mode(WAL)&_pragma=foreign_keys(1)&_pragma=busy_timeout(5000)")
	if err != nil {
		return nil, err
	}
	s := &Store{db: db}
	return s, s.migrate()
}

func (s *Store) Close() error { return s.db.Close() }

func (s *Store) migrate() error {
	_, err := s.db.Exec(`
CREATE TABLE IF NOT EXISTS devices(
  device_id TEXT PRIMARY KEY,
  token TEXT NOT NULL DEFAULT '',
  muted INTEGER NOT NULL DEFAULT 0,
  resume_room_id INTEGER
);
CREATE TABLE IF NOT EXISTS nfc_entries(
  entry_id TEXT PRIMARY KEY,
  kind TEXT NOT NULL CHECK(kind IN ('room','device')),
  room_id INTEGER,
  device_id TEXT
);
CREATE TABLE IF NOT EXISTS rooms(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE,
  invite_code TEXT NOT NULL UNIQUE,
  janus_room INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS memberships(
  device_id TEXT NOT NULL,
  room_id INTEGER NOT NULL,
  joined_at TEXT NOT NULL DEFAULT (datetime('now')),
  PRIMARY KEY(device_id, room_id)
);`)
	return err
}

func (s *Store) UpsertDevice(ctx context.Context, id, token string) (*Device, error) {
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO devices(device_id,token) VALUES(?,?)
		 ON CONFLICT(device_id) DO UPDATE SET token=excluded.token WHERE excluded.token<>''`, id, token)
	if err != nil {
		return nil, err
	}
	return s.GetDevice(ctx, id)
}

func (s *Store) GetDevice(ctx context.Context, id string) (*Device, error) {
	d := &Device{ID: id}
	var muted int
	err := s.db.QueryRowContext(ctx,
		`SELECT token, muted, resume_room_id FROM devices WHERE device_id=?`, id).
		Scan(&d.Token, &muted, &d.ResumeRoomID)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	d.Muted = muted != 0
	return d, nil
}

func (s *Store) SetMuted(ctx context.Context, id string, muted bool) error {
	m := 0
	if muted {
		m = 1
	}
	_, err := s.db.ExecContext(ctx, `UPDATE devices SET muted=? WHERE device_id=?`, m, id)
	return err
}

func (s *Store) SetResumeRoom(ctx context.Context, id string, roomID *int64) error {
	var v any
	if roomID != nil {
		v = *roomID
	}
	_, err := s.db.ExecContext(ctx, `UPDATE devices SET resume_room_id=? WHERE device_id=?`, v, id)
	return err
}

func (s *Store) GetRoom(ctx context.Context, id int64) (*Room, error) {
	r := &Room{ID: id}
	err := s.db.QueryRowContext(ctx,
		`SELECT name, invite_code, janus_room FROM rooms WHERE id=?`, id).
		Scan(&r.Name, &r.InviteCode, &r.JanusRoom)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	return r, err
}

func (s *Store) RoomByInvite(ctx context.Context, code string) (*Room, error) {
	r := &Room{}
	err := s.db.QueryRowContext(ctx,
		`SELECT id, name, invite_code, janus_room FROM rooms WHERE invite_code=?`, code).
		Scan(&r.ID, &r.Name, &r.InviteCode, &r.JanusRoom)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	return r, err
}

func (s *Store) RoomByName(ctx context.Context, name string) (*Room, error) {
	r := &Room{}
	err := s.db.QueryRowContext(ctx,
		`SELECT id, name, invite_code, janus_room FROM rooms WHERE name=?`, name).
		Scan(&r.ID, &r.Name, &r.InviteCode, &r.JanusRoom)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	return r, err
}

// CreateRoom inserts a room with generated name and invite code.
func (s *Store) CreateRoom(ctx context.Context, janusRoom int64) (*Room, error) {
	return s.createRoomTx(ctx, janusRoom, nil)
}

// CreateRoomWithEntry atomically creates a room and binds an NFC entry to it.
// If the entry already exists, returns the existing resolution.
func (s *Store) CreateRoomWithEntry(ctx context.Context, entryID string, janusRoomFn func(context.Context) (int64, error)) (*Room, bool, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, false, err
	}
	defer tx.Rollback()

	var kind string
	var roomID, devID sql.NullInt64
	var devStr sql.NullString
	err = tx.QueryRowContext(ctx,
		`SELECT kind, room_id, device_id FROM nfc_entries WHERE entry_id=?`, entryID).
		Scan(&kind, &roomID, &devStr)
	if err == nil {
		// existing entry
		if kind == "room" {
			r, err := roomByIDTx(ctx, tx, roomID.Int64)
			return r, false, err
		}
		return nil, false, nil // device entry handled by caller
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return nil, false, err
	}

	jr, err := janusRoomFn(ctx)
	if err != nil {
		return nil, false, err
	}
	r, err := createRoomTx(ctx, tx, jr)
	if err != nil {
		return nil, false, err
	}
	if _, err = tx.ExecContext(ctx,
		`INSERT INTO nfc_entries(entry_id,kind,room_id) VALUES(?,?,?)`,
		entryID, "room", r.ID); err != nil {
		return nil, false, err
	}
	_ = devID
	if err = tx.Commit(); err != nil {
		return nil, false, err
	}
	return r, true, nil
}

func (s *Store) createRoomTx(ctx context.Context, janusRoom int64, _ any) (*Room, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	r, err := createRoomTx(ctx, tx, janusRoom)
	if err != nil {
		return nil, err
	}
	return r, tx.Commit()
}

func createRoomTx(ctx context.Context, tx *sql.Tx, janusRoom int64) (*Room, error) {
	var maxID sql.NullInt64
	if err := tx.QueryRowContext(ctx, `SELECT MAX(id) FROM rooms`).Scan(&maxID); err != nil {
		return nil, err
	}
	name := RoomName(maxID.Int64 + 1)
	code, err := genInviteCodeTx(ctx, tx)
	if err != nil {
		return nil, err
	}
	res, err := tx.ExecContext(ctx,
		`INSERT INTO rooms(name,invite_code,janus_room) VALUES(?,?,?)`, name, code, janusRoom)
	if err != nil {
		return nil, err
	}
	id, _ := res.LastInsertId()
	return &Room{ID: id, Name: name, InviteCode: code, JanusRoom: janusRoom}, nil
}

func roomByIDTx(ctx context.Context, tx *sql.Tx, id int64) (*Room, error) {
	r := &Room{ID: id}
	err := tx.QueryRowContext(ctx,
		`SELECT name, invite_code, janus_room FROM rooms WHERE id=?`, id).
		Scan(&r.Name, &r.InviteCode, &r.JanusRoom)
	return r, err
}

func genInviteCodeTx(ctx context.Context, tx *sql.Tx) (string, error) {
	for range 100 {
		n, err := rand.Int(rand.Reader, big.NewInt(900000))
		if err != nil {
			return "", err
		}
		code := fmt.Sprintf("%06d", n.Int64()+100000)
		var cnt int
		if err := tx.QueryRowContext(ctx,
			`SELECT COUNT(1) FROM rooms WHERE invite_code=?`, code).Scan(&cnt); err != nil {
			return "", err
		}
		if cnt == 0 {
			return code, nil
		}
	}
	return "", errors.New("invite code space exhausted")
}

func (s *Store) LookupEntry(ctx context.Context, entryID string) (kind string, roomID *int64, deviceID string, err error) {
	var rid sql.NullInt64
	var did sql.NullString
	err = s.db.QueryRowContext(ctx,
		`SELECT kind, room_id, device_id FROM nfc_entries WHERE entry_id=?`, entryID).
		Scan(&kind, &rid, &did)
	if errors.Is(err, sql.ErrNoRows) {
		return "", nil, "", nil
	}
	if err != nil {
		return "", nil, "", err
	}
	if rid.Valid {
		roomID = &rid.Int64
	}
	return kind, roomID, did.String, nil
}

// RegisterDeviceEntry pre-registers a tag UID bound to a device.
func (s *Store) RegisterDeviceEntry(ctx context.Context, entryID, deviceID string) error {
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO nfc_entries(entry_id,kind,device_id) VALUES(?,?,?)
		 ON CONFLICT(entry_id) DO UPDATE SET kind='device', device_id=excluded.device_id, room_id=NULL`,
		entryID, "device", deviceID)
	return err
}

func (s *Store) AddMembership(ctx context.Context, deviceID string, roomID int64) error {
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO memberships(device_id,room_id) VALUES(?,?)
		 ON CONFLICT(device_id,room_id) DO NOTHING`, deviceID, roomID)
	return err
}

func (s *Store) MembershipRoomByName(ctx context.Context, deviceID, name string) (*Room, error) {
	r := &Room{}
	err := s.db.QueryRowContext(ctx,
		`SELECT r.id, r.name, r.invite_code, r.janus_room FROM memberships m
		 JOIN rooms r ON r.id=m.room_id WHERE m.device_id=? AND r.name=?`, deviceID, name).
		Scan(&r.ID, &r.Name, &r.InviteCode, &r.JanusRoom)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	return r, err
}

func (s *Store) Memberships(ctx context.Context, deviceID string) ([]Room, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT r.id, r.name, r.invite_code, r.janus_room FROM memberships m
		 JOIN rooms r ON r.id=m.room_id WHERE m.device_id=? ORDER BY r.id`, deviceID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Room
	for rows.Next() {
		var r Room
		if err := rows.Scan(&r.ID, &r.Name, &r.InviteCode, &r.JanusRoom); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

func InviteDigits(code string) string {
	return strings.Join(strings.Split(code, ""), " ")
}
