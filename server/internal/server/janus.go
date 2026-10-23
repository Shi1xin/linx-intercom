package server

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
)

// Participant is one terminal's media attachment on Janus.
type Participant struct {
	SessionID int64 `json:"session_id"`
	HandleID  int64 `json:"handle_id"`
	JanusRoom int64 `json:"janus_room"`
}

// Media abstracts the Janus AudioBridge signaling the control service adapts.
type Media interface {
	EnsureRoom(ctx context.Context, janusRoom int64) error
	Join(ctx context.Context, p *Participant, janusRoom int64, display string, offerSDP string) (answerSDP string, err error)
	ChangeRoom(ctx context.Context, p *Participant, janusRoom int64) error
	SetMuted(ctx context.Context, p *Participant, muted bool) error
	Leave(ctx context.Context, p *Participant) error
	Trickle(ctx context.Context, p *Participant, cand *IceCandidate, completed bool) error
	Detach(ctx context.Context, p *Participant) error
	// SetEventHandler registers a callback for async gateway frames
	// (trickle, webrtcup, hangup, media) keyed by session/handle id.
	SetEventHandler(func(sessionID, handleID int64, msg AsyncMsg))
	Close() error
}

// AsyncMsg is an unparsed Janus frame delivered out-of-transaction.
type AsyncMsg = janusMsg

// janusMsg is a decoded Janus protocol frame.
type janusMsg map[string]json.RawMessage

func (m janusMsg) str(k string) string {
	var s string
	_ = json.Unmarshal(m[k], &s)
	return s
}
func (m janusMsg) int(k string) int64 {
	var v struct {
		ID int64 `json:"id"`
	}
	if k == "data" {
		_ = json.Unmarshal(m[k], &v)
		return v.ID
	}
	var n int64
	_ = json.Unmarshal(m[k], &n)
	return n
}

// JanusClient is a shared Janus WebSocket connection with transaction routing.
type JanusClient struct {
	conn *websocket.Conn

	mu      sync.Mutex
	pending map[string]chan janusMsg
	txSeq   int64

	writeMu sync.Mutex

	OnAsync func(sessionID, handleID int64, msg janusMsg)
	OnDown  func() // called once when the connection is lost

	keepaliveTick time.Duration
	done          chan struct{}
	dead          atomic.Bool
	sessionsMu    sync.Mutex
	sessions      map[int64]struct{}
}

func DialJanus(ctx context.Context, url string) (*JanusClient, error) {
	conn, _, err := websocket.Dial(ctx, url, &websocket.DialOptions{
		Subprotocols:         []string{"janus-protocol"},
		HTTPHeader:           http.Header{},
		CompressionMode:      websocket.CompressionDisabled,
	})
	if err != nil {
		return nil, err
	}
	c := &JanusClient{
		conn:          conn,
		pending:       map[string]chan janusMsg{},
		done:          make(chan struct{}),
		sessions:      map[int64]struct{}{},
		keepaliveTick: 25 * time.Second,
	}
	go c.readLoop()
	go c.keepaliveLoop()
	return c, nil
}

func (c *JanusClient) nextTx() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.txSeq++
	return fmt.Sprintf("tx%d", c.txSeq)
}

func (c *JanusClient) readLoop() {
	defer close(c.done)
	for {
		_, data, err := c.conn.Read(context.Background())
		if err != nil {
			c.failAll()
			return
		}
		var m janusMsg
		if json.Unmarshal(data, &m) != nil {
			continue
		}
		tx := m.str("transaction")
		jt := m.str("janus")
		if tx != "" && jt != "ack" {
			c.mu.Lock()
			if ch, ok := c.pending[tx]; ok {
				delete(c.pending, tx)
				select {
				case ch <- m:
				default:
				}
				close(ch)
			}
			c.mu.Unlock()
			continue
		}
		if c.OnAsync != nil {
			c.OnAsync(m.int("session_id"), m.int("sender"), m)
		}
	}
}

func (c *JanusClient) failAll() {
	if c.dead.CompareAndSwap(false, true) && c.OnDown != nil {
		c.OnDown()
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	for tx, ch := range c.pending {
		delete(c.pending, tx)
		ch <- janusMsg{"janus": json.RawMessage(`"error"`), "error": json.RawMessage(`{"code":0,"reason":"connection lost"}`)}
		close(ch)
	}
}

func (c *JanusClient) keepaliveLoop() {
	t := time.NewTicker(c.keepaliveTick)
	defer t.Stop()
	for {
		select {
		case <-c.done:
			return
		case <-t.C:
			c.sessionsMu.Lock()
			for sid := range c.sessions {
				_ = c.send(map[string]any{
					"janus": "keepalive", "session_id": sid, "transaction": c.nextTx(),
				})
			}
			c.sessionsMu.Unlock()
		}
	}
}

func (c *JanusClient) send(v map[string]any) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return c.conn.Write(context.Background(), websocket.MessageText, data)
}

// Call sends a request and waits for the resolving frame (success/event/error).
func (c *JanusClient) Call(ctx context.Context, req map[string]any) (janusMsg, error) {
	tx := c.nextTx()
	req["transaction"] = tx
	ch := make(chan janusMsg, 1)
	c.mu.Lock()
	c.pending[tx] = ch
	c.mu.Unlock()
	if err := c.send(req); err != nil {
		c.mu.Lock()
		delete(c.pending, tx)
		c.mu.Unlock()
		return nil, err
	}
	select {
	case m := <-ch:
		if m.str("janus") == "error" {
			var e struct {
				Code   int64  `json:"code"`
				Reason string `json:"reason"`
			}
			_ = json.Unmarshal(m["error"], &e)
			return nil, fmt.Errorf("janus error %d: %s", e.Code, e.Reason)
		}
		return m, nil
	case <-ctx.Done():
		c.mu.Lock()
		delete(c.pending, tx)
		c.mu.Unlock()
		return nil, ctx.Err()
	}
}

// JanusMedia implements Media over Janus AudioBridge.
type JanusMedia struct {
	c        *JanusClient
	secret   string
	evtHdl   func(sessionID, handleID int64, msg AsyncMsg)
}

func NewJanusMedia(c *JanusClient, adminSecret string) *JanusMedia {
	jm := &JanusMedia{c: c, secret: adminSecret}
	c.OnAsync = func(sid, hid int64, m janusMsg) {
		if jm.evtHdl != nil {
			jm.evtHdl(sid, hid, m)
		}
	}
	return jm
}

func (j *JanusMedia) SetEventHandler(fn func(sessionID, handleID int64, msg AsyncMsg)) {
	j.evtHdl = fn
}

func (j *JanusMedia) EnsureRoom(ctx context.Context, janusRoom int64) error {
	// create a transient admin session to create the room idempotently
	p := &Participant{}
	if err := j.attach(ctx, p); err != nil {
		return err
	}
	defer j.detachSession(p)
	body := map[string]any{
		"request": "create", "room": janusRoom, "permanent": false,
		"sampling_rate": 16000, "audiolevel_ext": false, "record": false,
	}
	_, err := j.pluginCall(ctx, p, body, nil)
	var je *janusPluginErr
	if err != nil && errors.As(err, &je) && je.code == 486 {
		return nil // room exists
	}
	return err
}

type janusPluginErr struct{ code int64; reason string }
func (e *janusPluginErr) Error() string { return fmt.Sprintf("plugin error %d: %s", e.code, e.reason) }

// pluginCall sends a handle message and waits for the event frame.
func (j *JanusMedia) pluginCall(ctx context.Context, p *Participant, body map[string]any, jsep map[string]any) (janusMsg, error) {
	req := map[string]any{
		"janus": "message", "session_id": p.SessionID, "handle_id": p.HandleID,
		"body": body,
	}
	if jsep != nil {
		req["jsep"] = jsep
	}
	m, err := j.c.Call(ctx, req)
	if err != nil {
		return nil, err
	}
	var pd struct {
		Data struct {
			AB   string `json:"audiobridge"`
			Code int64  `json:"error_code"`
			Err  string `json:"error"`
		} `json:"plugindata"`
	}
	_ = json.Unmarshal(m["plugindata"], &pd)
	if pd.Data.Err != "" || pd.Data.Code != 0 {
		return nil, &janusPluginErr{code: pd.Data.Code, reason: pd.Data.Err}
	}
	return m, nil
}

func (j *JanusMedia) attach(ctx context.Context, p *Participant) error {
	m, err := j.c.Call(ctx, map[string]any{"janus": "create"})
	if err != nil {
		return err
	}
	p.SessionID = m.int("data")
	m, err = j.c.Call(ctx, map[string]any{
		"janus": "attach", "session_id": p.SessionID, "plugin": "janus.plugin.audiobridge",
	})
	if err != nil {
		return err
	}
	p.HandleID = m.int("data")
	j.c.sessionsMu.Lock()
	j.c.sessions[p.SessionID] = struct{}{}
	j.c.sessionsMu.Unlock()
	return nil
}

func (j *JanusMedia) detachSession(p *Participant) {
	j.c.sessionsMu.Lock()
	delete(j.c.sessions, p.SessionID)
	j.c.sessionsMu.Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, _ = j.c.Call(ctx, map[string]any{"janus": "destroy", "session_id": p.SessionID})
}

// Join attaches a fresh session and joins janusRoom with the SDP offer.
func (j *JanusMedia) Join(ctx context.Context, p *Participant, janusRoom int64, display string, offerSDP string) (string, error) {
	if err := j.attach(ctx, p); err != nil {
		return "", err
	}
	p.JanusRoom = janusRoom
	m, err := j.pluginCall(ctx, p, map[string]any{
		"request": "join", "room": janusRoom, "display": display, "muted": true,
	}, map[string]any{"type": "offer", "sdp": offerSDP})
	if err != nil {
		j.detachSession(p)
		return "", err
	}
	var js struct {
		Type string `json:"type"`
		SDP  string `json:"sdp"`
	}
	if err := json.Unmarshal(m["jsep"], &js); err != nil || js.SDP == "" {
		j.detachSession(p)
		return "", errors.New("join: no jsep answer")
	}
	return js.SDP, nil
}

func (j *JanusMedia) ChangeRoom(ctx context.Context, p *Participant, janusRoom int64) error {
	_, err := j.pluginCall(ctx, p, map[string]any{
		"request": "changeroom", "room": janusRoom,
	}, nil)
	if err != nil {
		return err
	}
	p.JanusRoom = janusRoom
	return nil
}

func (j *JanusMedia) SetMuted(ctx context.Context, p *Participant, muted bool) error {
	_, err := j.pluginCall(ctx, p, map[string]any{
		"request": "configure", "muted": muted,
	}, nil)
	return err
}

func (j *JanusMedia) Leave(ctx context.Context, p *Participant) error {
	_, err := j.pluginCall(ctx, p, map[string]any{"request": "leave"}, nil)
	return err
}

func (j *JanusMedia) Trickle(ctx context.Context, p *Participant, cand *IceCandidate, completed bool) error {
	var cval any
	if completed || cand == nil {
		cval = map[string]any{"completed": true}
	} else {
		cval = map[string]any{
			"candidate":     cand.Candidate,
			"sdpMid":        cand.SDPMid,
			"sdpMLineIndex": cand.SDPMLineIndex,
		}
	}
	return j.c.send(map[string]any{
		"janus": "trickle", "session_id": p.SessionID, "handle_id": p.HandleID,
		"candidate": cval, "transaction": j.c.nextTx(),
	})
}

func (j *JanusMedia) Detach(ctx context.Context, p *Participant) error {
	j.detachSession(p)
	return nil
}

func (j *JanusMedia) Close() error { return j.c.conn.Close(websocket.StatusNormalClosure, "bye") }

// LiveMedia wraps JanusMedia with lazy reconnect: when the Janus
// connection drops it redials on the next call and notifies the owner
// once so stale participants can be cleared.
type LiveMedia struct {
	mu     sync.Mutex
	dial   func(context.Context) (*JanusClient, error)
	jm     *JanusMedia
	onDown func()
	evtHdl func(sessionID, handleID int64, msg AsyncMsg)
}

func NewLiveMedia(dial func(context.Context) (*JanusClient, error), onDown func()) *LiveMedia {
	return &LiveMedia{dial: dial, onDown: onDown}
}

func (l *LiveMedia) media(ctx context.Context) (*JanusMedia, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.jm != nil && !l.jm.c.dead.Load() {
		return l.jm, nil
	}
	if l.jm != nil && l.onDown != nil {
		l.onDown()
	}
	jc, err := l.dial(ctx)
	if err != nil {
		return nil, err
	}
	l.jm = NewJanusMedia(jc, "")
	if l.evtHdl != nil {
		l.jm.SetEventHandler(l.evtHdl)
	}
	return l.jm, nil
}

func (l *LiveMedia) EnsureRoom(ctx context.Context, r int64) error {
	m, err := l.media(ctx); if err != nil { return err }; return m.EnsureRoom(ctx, r)
}
func (l *LiveMedia) Join(ctx context.Context, p *Participant, r int64, d, o string) (string, error) {
	m, err := l.media(ctx); if err != nil { return "", err }; return m.Join(ctx, p, r, d, o)
}
func (l *LiveMedia) ChangeRoom(ctx context.Context, p *Participant, r int64) error {
	m, err := l.media(ctx); if err != nil { return err }; return m.ChangeRoom(ctx, p, r)
}
func (l *LiveMedia) SetMuted(ctx context.Context, p *Participant, muted bool) error {
	m, err := l.media(ctx); if err != nil { return err }; return m.SetMuted(ctx, p, muted)
}
func (l *LiveMedia) Leave(ctx context.Context, p *Participant) error {
	m, err := l.media(ctx); if err != nil { return err }; return m.Leave(ctx, p)
}
func (l *LiveMedia) Trickle(ctx context.Context, p *Participant, c *IceCandidate, done bool) error {
	m, err := l.media(ctx); if err != nil { return err }; return m.Trickle(ctx, p, c, done)
}
func (l *LiveMedia) Detach(ctx context.Context, p *Participant) error {
	m, err := l.media(ctx); if err != nil { return err }; return m.Detach(ctx, p)
}
func (l *LiveMedia) SetEventHandler(fn func(sessionID, handleID int64, msg AsyncMsg)) {
	l.evtHdl = fn
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.jm != nil {
		l.jm.SetEventHandler(fn)
	}
}
func (l *LiveMedia) Close() error {
	l.mu.Lock(); defer l.mu.Unlock()
	if l.jm != nil { return l.jm.Close() }
	return nil
}
