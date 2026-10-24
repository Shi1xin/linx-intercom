package server

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

func TestRoomName(t *testing.T) {
	cases := map[int64]string{
		1: "一号房", 2: "二号房", 9: "九号房", 10: "十号房", 11: "十一号房",
		20: "二十号房", 21: "二十一号房", 99: "九十九号房", 100: "一百号房",
		101: "一百零一号房", 110: "一百一十号房", 115: "一百一十五号房", 200: "二百号房",
	}
	for id, want := range cases {
		if got := RoomName(id); got != want {
			t.Errorf("RoomName(%d) = %q, want %q", id, got, want)
		}
	}
}

func newTestStore(t *testing.T) *Store {
	t.Helper()
	s, err := OpenStore(filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	return s
}

func TestStoreRoomLifecycle(t *testing.T) {
	ctx := context.Background()
	s := newTestStore(t)

	dev, err := s.UpsertDevice(ctx, "aa:bb:cc:dd:ee:ff", "tok")
	if err != nil || dev == nil {
		t.Fatalf("upsert: %v", err)
	}
	if err := s.SetMuted(ctx, dev.ID, true); err != nil {
		t.Fatal(err)
	}
	dev, _ = s.GetDevice(ctx, dev.ID)
	if !dev.Muted {
		t.Fatal("muted not persisted")
	}

	r1, err := s.CreateRoom(ctx)
	if err != nil {
		t.Fatal(err)
	}
	r2, err := s.CreateRoom(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if r1.Name != "一号房" || r2.Name != "二号房" {
		t.Fatalf("names: %q %q", r1.Name, r2.Name)
	}
	if r1.InviteCode == r2.InviteCode {
		t.Fatal("invite codes collide")
	}
	if len(r1.InviteCode) != 6 {
		t.Fatalf("invite code %q not 6 digits", r1.InviteCode)
	}

	got, err := s.RoomByInvite(ctx, r1.InviteCode)
	if err != nil || got == nil || got.ID != r1.ID {
		t.Fatalf("RoomByInvite: %v %v", got, err)
	}

	if err := s.AddMembership(ctx, dev.ID, r1.ID); err != nil {
		t.Fatal(err)
	}
	if err := s.AddMembership(ctx, dev.ID, r1.ID); err != nil {
		t.Fatal("dup membership should be no-op")
	}
	mr, err := s.MembershipRoomByName(ctx, dev.ID, "一号房")
	if err != nil || mr == nil || mr.ID != r1.ID {
		t.Fatalf("MembershipRoomByName: %v %v", mr, err)
	}
	if mr, _ := s.MembershipRoomByName(ctx, dev.ID, "二号房"); mr != nil {
		t.Fatal("non-member room should be nil")
	}
}

func TestEntryCreateIdempotent(t *testing.T) {
	ctx := context.Background()
	s := newTestStore(t)

	r1, isNew, err := s.CreateRoomWithEntry(ctx, "tag-001")
	if err != nil || !isNew || r1 == nil {
		t.Fatalf("first create: %v new=%v err=%v", r1, isNew, err)
	}
	if r1.JanusRoom != janusRoomBase+r1.ID {
		t.Fatalf("janus room derivation: %d", r1.JanusRoom)
	}
	r2, isNew, err := s.CreateRoomWithEntry(ctx, "tag-001")
	if err != nil || isNew || r2 == nil || r2.ID != r1.ID {
		t.Fatalf("second tap: %v new=%v err=%v", r2, isNew, err)
	}

	kind, roomID, _, err := s.LookupEntry(ctx, "tag-001")
	if err != nil || kind != "room" || roomID == nil || *roomID != r1.ID {
		t.Fatalf("entry lookup: kind=%q room=%v err=%v", kind, roomID, err)
	}
}

func TestResumePersistence(t *testing.T) {
	ctx := context.Background()
	s := newTestStore(t)
	dev, _ := s.UpsertDevice(ctx, "d1", "")
	r, _ := s.CreateRoom(ctx)
	if err := s.SetResumeRoom(ctx, dev.ID, &r.ID); err != nil {
		t.Fatal(err)
	}
	dev, _ = s.GetDevice(ctx, dev.ID)
	if !dev.ResumeRoomID.Valid || dev.ResumeRoomID.Int64 != r.ID {
		t.Fatal("resume room not persisted")
	}
	if err := s.SetResumeRoom(ctx, dev.ID, nil); err != nil {
		t.Fatal(err)
	}
	dev, _ = s.GetDevice(ctx, dev.ID)
	if dev.ResumeRoomID.Valid {
		t.Fatal("resume room not cleared")
	}
}

// wsClient is a minimal protocol client over a test connection.
type wsClient struct {
	conn *websocket.Conn
	t    *testing.T
	seq  int
}

func newWS(t *testing.T, url string, devID string) *wsClient {
	t.Helper()
	conn, _, err := websocket.Dial(context.Background(), url, nil)
	if err != nil {
		t.Fatal(err)
	}
	c := &wsClient{conn: conn, t: t}
	c.t.Cleanup(func() { conn.Close(websocket.StatusNormalClosure, "") })
	c.send(InMessage{Type: "hello", DeviceID: devID, RequestID: "h1"})
	m := c.recv()
	if m.Type != "hello_result" || !m.OK {
		t.Fatalf("hello: %+v", m)
	}
	return c
}

func (c *wsClient) send(m InMessage) {
	c.seq++
	if m.RequestID == "" {
		m.RequestID = "auto"
	}
	data, _ := json.Marshal(m)
	if err := c.conn.Write(context.Background(), websocket.MessageText, data); err != nil {
		c.t.Fatal(err)
	}
}

func (c *wsClient) recv() *OutMessage {
	c.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, data, err := c.conn.Read(ctx)
	if err != nil {
		c.t.Fatal(err)
	}
	var m OutMessage
	if err := json.Unmarshal(data, &m); err != nil {
		c.t.Fatal(err)
	}
	return &m
}

func (c *wsClient) recvType(want string) *OutMessage {
	c.t.Helper()
	for range 20 {
		m := c.recv()
		if m.Type == want {
			return m
		}
	}
	c.t.Fatalf("no message of type %s", want)
	return nil
}

func (c *wsClient) joinRoom(t *testing.T, target JoinTarget) *OutMessage {
	c.seq++
	id := c.seq
	c.send(InMessage{Type: "join", RequestID: fmt.Sprintf("j%d", id), Target: &target})
	m := c.recvType("join_result")
	if !m.OK {
		t.Fatalf("join_result: %+v", m)
	}
	if m.NeedMedia {
		c.send(InMessage{Type: "sdp", RequestID: fmt.Sprintf("s%d", id), SDP: "fake offer"})
		m = c.recvType("sdp")
		if m.Jsep != "answer" {
			t.Fatalf("sdp: %+v", m)
		}
	}
	c.send(InMessage{Type: "confirm", RequestID: fmt.Sprintf("c%d", id)})
	m = c.recvType("confirm_result")
	return m
}

func testServer(t *testing.T) (*ControlServer, *FakeMedia, string) {
	t.Helper()
	store := newTestStore(t)
	fm := NewFakeMedia()
	srv := NewControlServer(Config{Addr: ":0"}, store, fm)
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)
	return srv, fm, strings.Replace(ts.URL, "http", "ws", 1) + "/ws"
}

func TestFullJoinFlow(t *testing.T) {
	_, fm, url := testServer(t)
	c := newWS(t, url, "dev-a")

	m := c.joinRoom(t, JoinTarget{Kind: "nfc", EntryID: "tag-room-1"})
	if m.Room.Name != "一号房" || m.Room.InviteCode == "" {
		t.Fatalf("confirm_result room: %+v", m.Room)
	}
	code := m.Room.InviteCode

	// second device joins same room by invite
	c2 := newWS(t, url, "dev-b")
	m2 := c2.joinRoom(t, JoinTarget{Kind: "invite", Code: code})
	if m2.Room.ID != m.Room.ID || m2.Room.OnlineCount != 2 {
		t.Fatalf("second member: %+v", m2.Room)
	}

	// room info
	c2.send(InMessage{Type: "room_info", RequestID: "i1"})
	ri := c2.recvType("room_info_result")
	if !ri.OK || ri.Name != "一号房" || ri.OnlineCount != 2 || ri.InviteDigits != InviteDigits(code) {
		t.Fatalf("room_info: %+v", ri)
	}

	// mute
	tru := true
	c2.send(InMessage{Type: "set_mute", RequestID: "m1", Muted: &tru})
	mr := c2.recvType("mute_result")
	if !mr.OK || !mr.Muted {
		t.Fatalf("mute: %+v", mr)
	}
	if !strings.Contains(strings.Join(fm.CallsSnapshot(), " "), "mute true") {
		t.Fatalf("janus mute not applied: %v", fm.CallsSnapshot())
	}

	// switch: c2 joins a second room via nfc then back by name
	m3 := c2.joinRoom(t, JoinTarget{Kind: "nfc", EntryID: "tag-room-2"})
	if m3.Room.Name != "二号房" {
		t.Fatalf("expected 二号房, got %+v", m3.Room)
	}
	c2.send(InMessage{Type: "join", RequestID: "jsw", Target: &JoinTarget{Kind: "room_name", Name: "一号房"}})
	sw := c2.recvType("join_result")
	if !sw.OK || sw.NeedMedia || sw.Room.Name != "一号房" {
		t.Fatalf("switch: %+v", sw)
	}
	c2.send(InMessage{Type: "confirm", RequestID: "c2"})
	cf := c2.recvType("confirm_result")
	if !cf.OK || cf.Room.Name != "一号房" {
		t.Fatalf("confirm after switch: %+v", cf)
	}
	if !strings.Contains(strings.Join(fm.CallsSnapshot(), " "), "changeroom") {
		t.Fatalf("expected changeroom call: %v", fm.CallsSnapshot())
	}

	// join unjoined room by name must fail
	c.send(InMessage{Type: "join", RequestID: "j9", Target: &JoinTarget{Kind: "room_name", Name: "二号房"}})
	mj := c.recvType("join_result")
	if mj.OK || mj.Error == nil || mj.Error.Code != "room_not_joined" {
		t.Fatalf("unjoined room join: %+v", mj)
	}

	// invalid invite
	c.send(InMessage{Type: "join", RequestID: "j8", Target: &JoinTarget{Kind: "invite", Code: "000000"}})
	mj = c.recvType("join_result")
	if mj.OK || mj.Error.Code != "invalid_invite" {
		t.Fatalf("invalid invite: %+v", mj)
	}

	// request id idempotency: same request_id returns cached result
	c.send(InMessage{Type: "join", RequestID: "dup1", Target: &JoinTarget{Kind: "nfc", EntryID: "tag-x"}})
	m1 := c.recvType("join_result")
	c.send(InMessage{Type: "join", RequestID: "dup1", Target: &JoinTarget{Kind: "nfc", EntryID: "tag-x"}})
	mdup := c.recvType("join_result")
	if m1.Room.ID != mdup.Room.ID {
		t.Fatalf("idempotent join diverged: %v vs %v", m1.Room, mdup.Room)
	}

	// leave: resume cleared, membership kept
	c2.send(InMessage{Type: "leave", RequestID: "l1"})
	lv := c2.recvType("leave_result")
	if !lv.OK {
		t.Fatalf("leave: %+v", lv)
	}
}

func TestDeviceTagSummon(t *testing.T) {
	srv, _, url := testServer(t)
	ctx := context.Background()
	if err := srv.store.RegisterDeviceEntry(ctx, "tag-dev-b", "dev-b"); err != nil {
		t.Fatal(err)
	}
	cb := newWS(t, url, "dev-b") // target online, no room
	ca := newWS(t, url, "dev-a")

	ca.send(InMessage{Type: "join", RequestID: "j1", Target: &JoinTarget{Kind: "nfc", EntryID: "tag-dev-b"}})
	m := ca.recvType("join_result")
	if !m.OK || !m.NeedMedia {
		t.Fatalf("device tag join: %+v", m)
	}
	// dev-b gets summon notify
	n := cb.recvType("notify")
	if n.Event != "summon" || n.Room == nil || n.Room.ID != m.Room.ID {
		t.Fatalf("summon: %+v", n)
	}
}

func TestResumeAfterReconnect(t *testing.T) {
	_, _, url := testServer(t)
	c := newWS(t, url, "dev-r")
	m := c.joinRoom(t, JoinTarget{Kind: "nfc", EntryID: "tag-r"})
	c.conn.Close(websocket.StatusNormalClosure, "reconnect")

	c2 := newWS(t, url, "dev-r")
	// newWS already consumed hello_result; check resume via a fresh hello read
	_ = m
	// hello_result is consumed inside newWS; verify resume room by re-hello
	c2.send(InMessage{Type: "hello", DeviceID: "dev-r", RequestID: "h2"})
	h := c2.recvType("hello_result")
	if h.ResumeRoom == nil || h.ResumeRoom.Name != "一号房" {
		t.Fatalf("resume: %+v", h.ResumeRoom)
	}
}

func TestChangeRoomFailureRestores(t *testing.T) {
	_, fm, url := testServer(t)
	c := newWS(t, url, "dev-f")
	c.joinRoom(t, JoinTarget{Kind: "nfc", EntryID: "tag-a"})
	c2room := c.joinRoom(t, JoinTarget{Kind: "nfc", EntryID: "tag-b"}) // second room via fresh media? no — participant exists, so changeroom path
	_ = c2room

	// force changeroom failure
	fm.SetFailNext(errTest)
	c.send(InMessage{Type: "join", RequestID: "jf", Target: &JoinTarget{Kind: "room_name", Name: "一号房"}})
	// currently in 二号房 after join; switching to 一号房 fails
	m := c.recvType("join_result")
	if m.OK || m.Error.Code != "media_failed" {
		t.Fatalf("expected media_failed: %+v", m)
	}
	// still in 二号房
	c.send(InMessage{Type: "room_info", RequestID: "i"})
	ri := c.recvType("room_info_result")
	if ri.Name != "二号房" {
		t.Fatalf("room after failed switch: %+v", ri)
	}
}

var errTest = &OutError{Code: "test", Message: "forced"}
