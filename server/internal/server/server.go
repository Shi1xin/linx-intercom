package server

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net/http"
	"sync"
	"time"

	"github.com/coder/websocket"
)

type Config struct {
	Addr        string // listen address, e.g. ":8080"
	JanusURL    string // ws://host:8188
	DeviceToken string // accepted device token; empty = accept any
	AdminToken  string // token for /admin endpoints
}

type ControlServer struct {
	cfg   Config
	store *Store
	media Media

	mu        sync.Mutex
	sessions  map[*Session]struct{}
	byDevice  map[string]*Session
	byHandle  map[int64]*Session
	orphanICE map[int64][]pendingICE
	mcp       *MCPAdapter
}

type Session struct {
	srv      *ControlServer
	conn     *websocket.Conn
	deviceID string
	muted    bool

	mu            sync.Mutex
	writeMu       sync.Mutex
	participant   *Participant
	roomID        int64
	pendingRoomID int64
	pendingICE    []pendingICE
	janusICE      []pendingICE
	answerSent    bool
	joinResps     map[string]*OutMessage // request_id dedup
	pendingOps    map[string]chan *InMessage // op_id -> result waiter
	closed        bool
}

type pendingICE struct {
	cand      *IceCandidate
	completed bool
}

func NewControlServer(cfg Config, store *Store, media Media) *ControlServer {
	s := &ControlServer{
		cfg:      cfg,
		store:    store,
		media:    media,
		sessions: map[*Session]struct{}{},
		byDevice: map[string]*Session{},
		byHandle: map[int64]*Session{},
		orphanICE: map[int64][]pendingICE{},
	}
	media.SetEventHandler(s.onMediaEvent)
	return s
}

// onMediaEvent routes async Janus frames (trickle, webrtcup, hangup) to sessions.
func (s *ControlServer) onMediaEvent(sessionID, handleID int64, m AsyncMsg) {
	if m.str("janus") != "trickle" {
		s.mu.Lock()
		sess := s.byHandle[handleID]
		s.mu.Unlock()
		if sess == nil {
			return
		}
		switch m.str("janus") {
		case "webrtcup":
			log.Printf("device %s: webrtc up", sess.deviceID)
		case "hangup":
			log.Printf("device %s: media hangup", sess.deviceID)
			_, roomID := s.clearMedia(sess)
			sess.send(&OutMessage{Type: "notify", Event: "media_lost"})
			if roomID != 0 {
				s.broadcastPeerChange(roomID, "peer_left", sess.deviceID)
			}
		}
		return
	}
	var cand struct {
		Candidate     string `json:"candidate"`
		SDPMid        string `json:"sdpMid"`
		SDPMLineIndex int    `json:"sdpMLineIndex"`
		Completed     bool   `json:"completed"`
	}
	_ = json.Unmarshal(m["candidate"], &cand)
	q := pendingICE{completed: cand.Completed}
	if !cand.Completed && cand.Candidate != "" {
		q.cand = &IceCandidate{
			Candidate: cand.Candidate, SDPMid: cand.SDPMid, SDPMLineIndex: cand.SDPMLineIndex,
		}
	} else if !cand.Completed {
		return
	}
	s.mu.Lock()
	sess := s.byHandle[handleID]
	if sess == nil {
		if len(s.orphanICE[handleID]) < 64 {
			s.orphanICE[handleID] = append(s.orphanICE[handleID], q)
		}
		s.mu.Unlock()
		return
	}
	s.mu.Unlock()
	sess.mu.Lock()
	if !sess.answerSent {
		sess.janusICE = append(sess.janusICE, q)
		sess.mu.Unlock()
		return
	}
	sess.mu.Unlock()
	if q.completed {
		sess.send(&OutMessage{Type: "ice", Completed: true})
	} else {
		sess.send(&OutMessage{Type: "ice", Candidate: q.cand})
	}
}

// AttachMCP registers the cloud MCP (SSE) endpoints on the server handler.
func (s *ControlServer) AttachMCP(defaultTarget, token string) {
	s.mcp = NewMCPAdapter(s, defaultTarget, token)
}

func (s *ControlServer) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", s.handleWS)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ok"))
	})
	mux.HandleFunc("/admin/register-entry", s.handleRegisterEntry)
	mux.HandleFunc("/admin/rooms", s.handleRooms)
	mux.HandleFunc("/admin/op", s.handleAdminOp)
	if s.mcp != nil {
		s.mcp.Register(mux)
	}
	mux.Handle("/", http.FileServer(http.Dir("tools/browser")))
	return mux
}

func (s *ControlServer) handleRegisterEntry(w http.ResponseWriter, r *http.Request) {
	if s.cfg.AdminToken != "" && r.Header.Get("X-Admin-Token") != s.cfg.AdminToken {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	var req struct {
		EntryID  string `json:"entry_id"`
		DeviceID string `json:"device_id"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.EntryID == "" || req.DeviceID == "" {
		http.Error(w, "bad request", http.StatusBadRequest)
		return
	}
	if err := s.store.RegisterDeviceEntry(r.Context(), req.EntryID, req.DeviceID); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(`{"ok":true}`))
}

// handleAdminOp dispatches a server-initiated op to a device and returns the
// device's op_result verbatim. Used by the mcpproxy bridge and admin scripts.
func (s *ControlServer) handleAdminOp(w http.ResponseWriter, r *http.Request) {
	if s.cfg.AdminToken != "" && r.Header.Get("X-Admin-Token") != s.cfg.AdminToken {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		DeviceID string `json:"device_id"`
		Op       string `json:"op"`
		Arg      string `json:"arg"`
		Muted    *bool  `json:"muted"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.DeviceID == "" || req.Op == "" {
		http.Error(w, "bad request", http.StatusBadRequest)
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	res, err := s.DispatchOp(ctx, req.DeviceID, req.Op, req.Arg, req.Muted)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		if oe, ok := err.(*OutError); ok {
			_ = json.NewEncoder(w).Encode(map[string]any{"ok": false, "error": oe})
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": false, "error": err.Error()})
		return
	}
	_ = json.NewEncoder(w).Encode(res)
}

func (s *ControlServer) handleRooms(w http.ResponseWriter, r *http.Request) {
	if s.cfg.AdminToken != "" && r.Header.Get("X-Admin-Token") != s.cfg.AdminToken {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(`{"ok":true}`))
}

func (s *ControlServer) handleWS(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		InsecureSkipVerify: true, // same-origin check off; test tool served cross-origin
	})
	if err != nil {
		log.Printf("ws accept: %v", err)
		return
	}
	conn.SetReadLimit(1 << 20)
	sess := &Session{srv: s, conn: conn, joinResps: map[string]*OutMessage{}}
	defer conn.Close(websocket.StatusNormalClosure, "")
	// liveness: ping every 30s, dead conn triggers onDisconnect
	pingDone := make(chan struct{})
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-pingDone:
				return
			case <-t.C:
				ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
				err := conn.Ping(ctx)
				cancel()
				if err != nil {
					conn.Close(websocket.StatusGoingAway, "ping timeout")
					return
				}
			}
		}
	}()
	defer close(pingDone)
	for {
		_, data, err := conn.Read(context.Background())
		if err != nil {
			s.onDisconnect(sess)
			return
		}
		var m InMessage
		if err := json.Unmarshal(data, &m); err != nil {
			sess.send(errOut("", "bad_request", "invalid json"))
			continue
		}
		s.dispatch(sess, &m)
	}
}

func (s *ControlServer) dispatch(sess *Session, m *InMessage) {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	switch m.Type {
	case "hello":
		s.onHello(ctx, sess, m)
	case "join":
		s.onJoin(ctx, sess, m)
	case "sdp":
		s.onSDP(ctx, sess, m)
	case "ice":
		s.onICE(ctx, sess, m)
	case "confirm":
		s.onConfirm(ctx, sess, m)
	case "room_info":
		s.onRoomInfo(ctx, sess, m)
	case "set_mute":
		s.onSetMute(ctx, sess, m)
	case "leave":
		s.onLeave(ctx, sess, m)
	case "ping":
		sess.send(&OutMessage{Type: "pong", RequestID: m.RequestID})
	case "op_result":
		s.onOpResult(sess, m)
	default:
		sess.send(errOut(m.RequestID, "bad_request", "unknown type "+m.Type))
	}
}

func (sess *Session) send(m *OutMessage) error {
	data, err := json.Marshal(m)
	if err != nil {
		return err
	}
	sess.writeMu.Lock()
	defer sess.writeMu.Unlock()
	if sess.closed {
		return errors.New("closed")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := sess.conn.Write(ctx, websocket.MessageText, data); err != nil {
		sess.closed = true
		return err
	}
	return nil
}

func (s *ControlServer) onHello(ctx context.Context, sess *Session, m *InMessage) {
	if m.DeviceID == "" {
		sess.send(errOut(m.RequestID, "bad_request", "device_id required"))
		return
	}
	if s.cfg.DeviceToken != "" && m.Token != s.cfg.DeviceToken {
		sess.send(&OutMessage{Type: "hello_result", RequestID: m.RequestID, OK: false,
			Error: &OutError{Code: "auth_failed", Message: "invalid token"}})
		return
	}
	dev, err := s.store.UpsertDevice(ctx, m.DeviceID, m.Token)
	if err != nil {
		sess.send(errOut(m.RequestID, "internal", err.Error()))
		return
	}
	if dev.Token != "" && m.Token != dev.Token {
		sess.send(&OutMessage{Type: "hello_result", RequestID: m.RequestID, OK: false,
			Error: &OutError{Code: "auth_failed", Message: "device token mismatch"}})
		return
	}
	s.mu.Lock()
	if sess.deviceID != "" && sess.deviceID != m.DeviceID {
		delete(s.byDevice, sess.deviceID)
	}
	s.mu.Unlock()
	sess.deviceID = m.DeviceID
	sess.muted = dev.Muted

	s.mu.Lock()
	if old := s.byDevice[m.DeviceID]; old != nil && old != sess {
		old.mu.Lock()
		old.closed = true
		old.mu.Unlock()
		_ = old.conn.Close(websocket.StatusNormalClosure, "superseded")
	}
	s.sessions[sess] = struct{}{}
	s.byDevice[m.DeviceID] = sess
	s.mu.Unlock()

	out := &OutMessage{Type: "hello_result", RequestID: m.RequestID, OK: true, Muted: dev.Muted}
	if dev.ResumeRoomID.Valid {
		if room, err := s.store.GetRoom(ctx, dev.ResumeRoomID.Int64); err == nil && room != nil {
			out.ResumeRoom = s.roomInfo(ctx, room)
		}
	}
	sess.send(out)
}

func (s *ControlServer) roomInfo(ctx context.Context, r *Room) *RoomInfo {
	return &RoomInfo{ID: r.ID, Name: r.Name, InviteCode: r.InviteCode,
		OnlineCount: s.onlineCount(r.ID)}
}

func (s *ControlServer) onlineCount(roomID int64) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	n := 0
	for sess := range s.sessions {
		sess.mu.Lock()
		if sess.roomID == roomID && sess.participant != nil {
			n++
		}
		sess.mu.Unlock()
	}
	return n
}

func (s *ControlServer) onlineMembers(roomID int64) []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []string
	for sess := range s.sessions {
		sess.mu.Lock()
		if sess.roomID == roomID && sess.participant != nil {
			out = append(out, sess.deviceID)
		}
		sess.mu.Unlock()
	}
	return out
}

// resolveTarget maps a join target to a room.
func (s *ControlServer) resolveTarget(ctx context.Context, sess *Session, t *JoinTarget) (*Room, error) {
	switch t.Kind {
	case "invite":
		room, err := s.store.RoomByInvite(ctx, t.Code)
		if err != nil {
			return nil, err
		}
		if room == nil {
			return nil, &OutError{Code: "invalid_invite", Message: "邀请码无效"}
		}
		return room, nil
	case "room_name":
		room, err := s.store.MembershipRoomByName(ctx, sess.deviceID, t.Name)
		if err != nil {
			return nil, err
		}
		if room == nil {
			return nil, &OutError{Code: "room_not_joined", Message: "未加入该房间"}
		}
		return room, nil
	case "nfc":
		kind, roomID, devID, err := s.store.LookupEntry(ctx, t.EntryID)
		if err != nil {
			return nil, err
		}
		if kind == "room" && roomID != nil {
			return s.store.GetRoom(ctx, *roomID)
		}
		if kind == "device" {
			return s.resolveDeviceTag(ctx, devID)
		}
		// unregistered tag: atomically create room + bind entry
		room, _, err := s.store.CreateRoomWithEntry(ctx, t.EntryID)
		if err != nil {
			return nil, err
		}
		if room == nil {
			return nil, &OutError{Code: "entry_ambiguous", Message: "标签类型冲突"}
		}
		if err := s.media.EnsureRoom(ctx, room.JanusRoom); err != nil {
			return nil, &OutError{Code: "media_failed", Message: err.Error()}
		}
		return room, nil
	case "device":
		devID := t.DeviceID
		if kind, _, bound, err := s.store.LookupEntry(ctx, devID); err == nil && kind == "device" && bound != "" {
			devID = bound
		}
		return s.resolveDeviceTag(ctx, devID)
	default:
		return nil, &OutError{Code: "bad_request", Message: "unknown target kind"}
	}
}

func (s *ControlServer) resolveDeviceTag(ctx context.Context, targetDev string) (*Room, error) {
	if targetDev == "" {
		return nil, &OutError{Code: "bad_request", Message: "device id required"}
	}
	s.mu.Lock()
	tsess := s.byDevice[targetDev]
	s.mu.Unlock()
	if tsess == nil {
		return nil, &OutError{Code: "target_offline", Message: "目标设备离线"}
	}
	tsess.mu.Lock()
	cur := tsess.roomID
	hasMedia := tsess.participant != nil
	tsess.mu.Unlock()
	if cur != 0 && hasMedia {
		return s.store.GetRoom(ctx, cur)
	}
	// target online, no current room: create a room and summon both
	room, err := s.store.CreateRoom(ctx)
	if err != nil {
		return nil, err
	}
	if err := s.media.EnsureRoom(ctx, room.JanusRoom); err != nil {
		return nil, err
	}
	// notify target device to join
	tsess.send(&OutMessage{Type: "notify", Event: "summon", Room: s.roomInfo(ctx, room)})
	return room, nil
}

func (s *ControlServer) onJoin(ctx context.Context, sess *Session, m *InMessage) {
	if sess.deviceID == "" {
		sess.send(errOut(m.RequestID, "bad_request", "hello first"))
		return
	}
	if m.Target == nil {
		sess.send(errOut(m.RequestID, "bad_request", "target required"))
		return
	}
	sess.mu.Lock()
	if prev, ok := sess.joinResps[m.RequestID]; ok {
		sess.mu.Unlock()
		sess.send(prev)
		return
	}
	sess.mu.Unlock()

	room, err := s.resolveTarget(ctx, sess, m.Target)
	if err != nil {
		var oe *OutError
		if errors.As(err, &oe) {
			sess.send(&OutMessage{Type: "join_result", RequestID: m.RequestID, OK: false, Error: oe})
		} else {
			sess.send(&OutMessage{Type: "join_result", RequestID: m.RequestID, OK: false,
				Error: &OutError{Code: "internal", Message: err.Error()}})
		}
		return
	}
	if err := s.store.AddMembership(ctx, sess.deviceID, room.ID); err != nil {
		log.Printf("membership: %v", err)
	}

	sess.mu.Lock()
	hasMedia := sess.participant != nil
	sess.mu.Unlock()

	if !hasMedia {
		if err := s.media.EnsureRoom(ctx, room.JanusRoom); err != nil {
			sess.send(&OutMessage{Type: "join_result", RequestID: m.RequestID, OK: false,
				Error: &OutError{Code: "media_failed", Message: err.Error()}})
			return
		}
		out := &OutMessage{Type: "join_result", RequestID: m.RequestID, OK: true,
			NeedMedia: true, Room: s.roomInfo(ctx, room)}
		sess.mu.Lock()
		sess.joinResps[m.RequestID] = out
		sess.pendingRoomID = room.ID
		sess.mu.Unlock()
		sess.send(out)
		return
	}

	// switch via changeroom
	sess.mu.Lock()
	p := sess.participant
	prevRoom := sess.roomID
	sess.mu.Unlock()
	if prevRoom == room.ID {
		out := &OutMessage{Type: "join_result", RequestID: m.RequestID, OK: true,
			NeedMedia: false, Room: s.roomInfo(ctx, room)}
		sess.send(out)
		return
	}
	if err := s.media.ChangeRoom(ctx, p, room.JanusRoom); err != nil {
		sess.send(&OutMessage{Type: "join_result", RequestID: m.RequestID, OK: false,
			Error: &OutError{Code: "media_failed", Message: "切房失败: " + err.Error()}})
		return
	}
	sess.mu.Lock()
	sess.roomID = room.ID
	sess.mu.Unlock()
	s.broadcastPeerChange(prevRoom, "peer_left", sess.deviceID)
	out := &OutMessage{Type: "join_result", RequestID: m.RequestID, OK: true,
		NeedMedia: false, Room: s.roomInfo(ctx, room)}
	sess.mu.Lock()
	sess.joinResps[m.RequestID] = out
	sess.mu.Unlock()
	sess.send(out)
}

func (s *ControlServer) onSDP(ctx context.Context, sess *Session, m *InMessage) {
	if sess.deviceID == "" {
		sess.send(errOut(m.RequestID, "bad_request", "hello first"))
		return
	}
	sess.mu.Lock()
	if sess.participant != nil {
		sess.mu.Unlock()
		sess.send(errOut(m.RequestID, "bad_request", "media already attached"))
		return
	}
	pendingID := sess.pendingRoomID
	sess.mu.Unlock()
	if pendingID == 0 {
		sess.send(errOut(m.RequestID, "bad_request", "no pending join"))
		return
	}
	room, err := s.store.GetRoom(ctx, pendingID)
	if err != nil || room == nil {
		sess.send(errOut(m.RequestID, "internal", "pending room missing"))
		return
	}
	p := &Participant{}
	answer, err := s.media.Join(ctx, p, room.JanusRoom, sess.deviceID, m.SDP)
	if err != nil {
		sess.send(&OutMessage{Type: "sdp", RequestID: m.RequestID, OK: false,
			Error: &OutError{Code: "media_failed", Message: err.Error()}})
		return
	}
	// register handle and drain any candidates that raced ahead of us
	s.mu.Lock()
	s.byHandle[p.HandleID] = sess
	orphans := s.orphanICE[p.HandleID]
	delete(s.orphanICE, p.HandleID)
	s.mu.Unlock()

	// send answer then queued janus candidates under sess.mu so a racing
	// trickle keeps wire order: sdp always precedes forwarded candidates
	sess.mu.Lock()
	sess.participant = p
	queued := sess.pendingICE
	sess.pendingICE = nil
	jq := append(orphans, sess.janusICE...)
	sess.janusICE = nil
	if err := sess.send(&OutMessage{Type: "sdp", RequestID: m.RequestID, Jsep: "answer", SDP: answer}); err == nil {
		sess.answerSent = true
	}
	for _, q := range jq {
		if q.completed {
			sess.send(&OutMessage{Type: "ice", Completed: true})
		} else {
			sess.send(&OutMessage{Type: "ice", Candidate: q.cand})
		}
	}
	sess.mu.Unlock()
	for _, q := range queued {
		_ = s.media.Trickle(ctx, p, q.cand, q.completed)
	}
}

func (s *ControlServer) onICE(ctx context.Context, sess *Session, m *InMessage) {
	sess.mu.Lock()
	p := sess.participant
	if p == nil {
		if len(sess.pendingICE) < 64 {
			sess.pendingICE = append(sess.pendingICE, pendingICE{cand: m.Candidate, completed: m.Completed})
		}
		sess.mu.Unlock()
		return
	}
	sess.mu.Unlock()
	_ = s.media.Trickle(ctx, p, m.Candidate, m.Completed)
}

func (s *ControlServer) onConfirm(ctx context.Context, sess *Session, m *InMessage) {
	sess.mu.Lock()
	roomID := sess.roomID
	if roomID == 0 {
		roomID = sess.pendingRoomID
	}
	p := sess.participant
	sess.mu.Unlock()
	if p == nil || roomID == 0 {
		sess.send(&OutMessage{Type: "confirm_result", RequestID: m.RequestID, OK: false,
			Error: &OutError{Code: "bad_request", Message: "no media session"}})
		return
	}
	sess.mu.Lock()
	sess.roomID = roomID
	sess.pendingRoomID = 0
	sess.mu.Unlock()
	room, err := s.store.GetRoom(ctx, roomID)
	if err != nil || room == nil {
		sess.send(errOut(m.RequestID, "internal", "room missing"))
		return
	}
	if err := s.store.SetResumeRoom(ctx, sess.deviceID, &roomID); err != nil {
		log.Printf("resume room: %v", err)
	}
	_ = s.store.AddMembership(ctx, sess.deviceID, roomID)
	s.broadcastPeerChange(roomID, "peer_joined", sess.deviceID)
	sess.send(&OutMessage{Type: "confirm_result", RequestID: m.RequestID, OK: true,
		Room: s.roomInfo(ctx, room)})
	// restore persisted mute setting on janus (participants join muted)
	sess.mu.Lock()
	muted := sess.muted
	sess.mu.Unlock()
	if err := s.media.SetMuted(context.Background(), p, muted); err != nil {
		log.Printf("device %s: restore mute %v: %v", sess.deviceID, muted, err)
	}
}

// DispatchOp sends a server-initiated op to a connected device and waits for
// its op_result. ops are serialized per call site by opID uniqueness; a second
// dispatch for the same op_id returns the first result (callers use fresh ids).
func (s *ControlServer) DispatchOp(ctx context.Context, deviceID, op, arg string, muted *bool) (*InMessage, error) {
	s.mu.Lock()
	sess := s.byDevice[deviceID]
	s.mu.Unlock()
	if sess == nil {
		return nil, &OutError{Code: "target_offline", Message: "目标设备离线"}
	}
	opID := fmt.Sprintf("op%d", time.Now().UnixNano())
	ch := make(chan *InMessage, 1)
	sess.mu.Lock()
	if sess.pendingOps == nil {
		sess.pendingOps = map[string]chan *InMessage{}
	}
	sess.pendingOps[opID] = ch
	sess.mu.Unlock()
	defer func() {
		sess.mu.Lock()
		delete(sess.pendingOps, opID)
		sess.mu.Unlock()
	}()
	if err := sess.send(&OutMessage{Type: "op", OpID: opID, Op: op, OpArg: arg, OpMute: muted}); err != nil {
		return nil, &OutError{Code: "send_failed", Message: err.Error()}
	}
	select {
	case res := <-ch:
		return res, nil
	case <-ctx.Done():
		return nil, &OutError{Code: "op_timeout", Message: "设备操作超时"}
	}
}

// normalizeOpResult lifts fields from an op_result whose payload is a
// JSON-encoded `message` string (firmware shape) so callers always see
// structured mode/room/muted regardless of device implementation.
func normalizeOpResult(m *InMessage) {
	if m.Mode != "" || len(m.OKMsg) == 0 || m.OKMsg[0] != '{' {
		return
	}
	var inner struct {
		OK    *bool   `json:"ok"`
		Mode  *string `json:"mode"`
		Muted *bool   `json:"muted"`
		Room  *string `json:"room"`
		Code  *string `json:"invite_code"`
		Count *int    `json:"online_count"`
		Err   *string `json:"error"`
		Msg   *string `json:"message"`
	}
	if json.Unmarshal([]byte(m.OKMsg), &inner) != nil {
		return
	}
	if inner.OK != nil {
		m.OK = *inner.OK
	}
	if inner.Mode != nil {
		m.Mode = *inner.Mode
	}
	if inner.Muted != nil {
		m.Muted = inner.Muted
	}
	if inner.Room != nil {
		m.Room = &RoomInfo{Name: *inner.Room}
		if inner.Code != nil {
			m.Room.InviteCode = *inner.Code
		}
		if inner.Count != nil {
			m.Room.OnlineCount = *inner.Count
		}
	}
	if !m.OK && m.Error == nil && (inner.Err != nil || inner.Msg != nil) {
		code, msg := "op_failed", ""
		if inner.Err != nil {
			code = *inner.Err
		}
		if inner.Msg != nil {
			msg = *inner.Msg
		}
		m.Error = &OutError{Code: code, Message: msg}
	}
}

func (s *ControlServer) onOpResult(sess *Session, m *InMessage) {
	normalizeOpResult(m)
	sess.mu.Lock()
	ch := sess.pendingOps[m.OpID]
	delete(sess.pendingOps, m.OpID)
	sess.mu.Unlock()
	if ch != nil {
		ch <- m
	}
}

func (s *ControlServer) broadcastPeerChange(roomID int64, event, deviceID string) {
	var targets []*Session
	s.mu.Lock()
	for sess := range s.sessions {
		sess.mu.Lock()
		inRoom := sess.roomID == roomID
		sess.mu.Unlock()
		if inRoom && sess.deviceID != deviceID {
			targets = append(targets, sess)
		}
	}
	s.mu.Unlock()
	for _, sess := range targets {
		sess.send(&OutMessage{Type: "notify", Event: event, DeviceID: deviceID})
	}
}

func (s *ControlServer) onRoomInfo(ctx context.Context, sess *Session, m *InMessage) {
	sess.mu.Lock()
	roomID := sess.roomID
	sess.mu.Unlock()
	if roomID == 0 {
		sess.send(&OutMessage{Type: "room_info_result", RequestID: m.RequestID, OK: false,
			Error: &OutError{Code: "not_in_room", Message: "当前不在房间"}})
		return
	}
	room, err := s.store.GetRoom(ctx, roomID)
	if err != nil || room == nil {
		sess.send(errOut(m.RequestID, "internal", "room missing"))
		return
	}
	sess.send(&OutMessage{Type: "room_info_result", RequestID: m.RequestID, OK: true,
		Name: room.Name, InviteCode: room.InviteCode, InviteDigits: InviteDigits(room.InviteCode),
		OnlineCount: s.onlineCount(roomID), Members: s.onlineMembers(roomID)})
}

func (s *ControlServer) onSetMute(ctx context.Context, sess *Session, m *InMessage) {
	if m.Muted == nil {
		sess.send(errOut(m.RequestID, "bad_request", "muted required"))
		return
	}
	if err := s.store.SetMuted(ctx, sess.deviceID, *m.Muted); err != nil {
		sess.send(errOut(m.RequestID, "internal", err.Error()))
		return
	}
	sess.mu.Lock()
	sess.muted = *m.Muted
	p := sess.participant
	sess.mu.Unlock()
	if p != nil {
		if err := s.media.SetMuted(ctx, p, *m.Muted); err != nil {
			sess.send(&OutMessage{Type: "mute_result", RequestID: m.RequestID, OK: false,
				Error: &OutError{Code: "media_failed", Message: err.Error()}})
			return
		}
	}
	sess.send(&OutMessage{Type: "mute_result", RequestID: m.RequestID, OK: true, Muted: *m.Muted})
}

func (s *ControlServer) clearMedia(sess *Session) (*Participant, int64) {
	sess.mu.Lock()
	p := sess.participant
	roomID := sess.roomID
	sess.participant = nil
	sess.roomID = 0
	sess.pendingRoomID = 0
	sess.pendingICE = nil
	sess.janusICE = nil
	sess.answerSent = false
	sess.joinResps = map[string]*OutMessage{}
	sess.mu.Unlock()
	if p != nil {
		s.mu.Lock()
		delete(s.byHandle, p.HandleID)
		s.mu.Unlock()
	}
	return p, roomID
}

func (s *ControlServer) onLeave(ctx context.Context, sess *Session, m *InMessage) {
	p, roomID := s.clearMedia(sess)
	_ = s.store.SetResumeRoom(ctx, sess.deviceID, nil)
	if p != nil {
		_ = s.media.Leave(ctx, p)
		_ = s.media.Detach(ctx, p)
	}
	s.broadcastPeerChange(roomID, "peer_left", sess.deviceID)
	sess.send(&OutMessage{Type: "leave_result", RequestID: m.RequestID, OK: true})
}

// ClearAllMedia drops every session's media state without detaching —
// used when the Janus connection is lost (participants die upstream).
// Devices keep resume_room and rejoin via the normal join flow.
func (s *ControlServer) ClearAllMedia() {
	s.mu.Lock()
	list := make([]*Session, 0, len(s.sessions))
	for sess := range s.sessions {
		list = append(list, sess)
	}
	s.byHandle = map[int64]*Session{}
	s.orphanICE = map[int64][]pendingICE{}
	s.mu.Unlock()
	for _, sess := range list {
		s.clearMedia(sess)
	}
	log.Printf("media cleared for %d sessions (janus down)", len(list))
}

func (s *ControlServer) onDisconnect(sess *Session) {
	s.mu.Lock()
	delete(s.sessions, sess)
	if s.byDevice[sess.deviceID] == sess {
		delete(s.byDevice, sess.deviceID)
	}
	s.mu.Unlock()
	p, roomID := s.clearMedia(sess)
	sess.mu.Lock()
	sess.closed = true
	for opID, ch := range sess.pendingOps { // fail in-flight ops so waiters return fast
		ch <- &InMessage{OpID: opID, OK: false,
			Error: &OutError{Code: "target_offline", Message: "设备连接断开"}}
	}
	sess.pendingOps = nil
	sess.mu.Unlock()
	if p != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = s.media.Detach(ctx, p)
	}
	s.broadcastPeerChange(roomID, "peer_left", sess.deviceID)
}

func (s *ControlServer) ListenAndServe() error {
	log.Printf("intercom control server listening on %s", s.cfg.Addr)
	return http.ListenAndServe(s.cfg.Addr, s.Handler())
}

var _ = fmt.Sprintf
