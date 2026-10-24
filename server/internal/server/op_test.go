package server

import (
	"bufio"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

// recvOp reads until an "op" message arrives and returns it.
func (c *wsClient) recvOp(t *testing.T) *OutMessage {
	t.Helper()
	return c.recvType("op")
}

func replyOp(t *testing.T, c *wsClient, m *OutMessage, ok bool, message string) {
	t.Helper()
	c.send(InMessage{Type: "op_result", OpID: m.OpID, OK: ok, OKMsg: message})
}

func TestOpDispatchSuccess(t *testing.T) {
	srv, _, url := testServer(t)
	c := newWS(t, url, "dev-op")

	resCh := make(chan *InMessage, 1)
	go func() {
		res, err := srv.DispatchOp(context.Background(), "dev-op", "status", "", nil)
		if err != nil {
			resCh <- &InMessage{Error: &OutError{Code: "err", Message: err.Error()}}
			return
		}
		resCh <- res
	}()

	m := c.recvOp(t)
	if m.Op != "status" || m.OpID == "" {
		t.Fatalf("op message: %+v", m)
	}
	// firmware-shaped reply: fields only inside the message JSON string
	replyOp(t, c, m, true,
		`{"ok":true,"mode":"intercom","room":"二号房","invite_code":"468911","online_count":3,"muted":true}`)
	res := <-resCh
	if !res.OK || res.Mode != "intercom" || res.Muted == nil || !*res.Muted {
		t.Fatalf("normalized result: %+v", res)
	}
	if res.Room == nil || res.Room.Name != "二号房" || res.Room.InviteCode != "468911" || res.Room.OnlineCount != 3 {
		t.Fatalf("room: %+v", res.Room)
	}
}

func TestOpDispatchOffline(t *testing.T) {
	srv, _, _ := testServer(t)
	_, err := srv.DispatchOp(context.Background(), "no-such-device", "status", "", nil)
	oe, ok := err.(*OutError)
	if !ok || oe.Code != "target_offline" {
		t.Fatalf("err: %v", err)
	}
}

func TestOpDispatchFailure(t *testing.T) {
	srv, _, url := testServer(t)
	c := newWS(t, url, "dev-fail")

	resCh := make(chan *InMessage, 1)
	go func() {
		res, _ := srv.DispatchOp(context.Background(), "dev-fail", "join", "999999", nil)
		resCh <- res
	}()

	m := c.recvOp(t)
	if m.Op != "join" || m.OpArg != "999999" {
		t.Fatalf("op message: %+v", m)
	}
	replyOp(t, c, m, false, `{"ok":false,"error":"join_failed","message":"邀请码无效"}`)
	res := <-resCh
	if res.OK || res.Error == nil || res.Error.Code != "join_failed" || res.Error.Message != "邀请码无效" {
		t.Fatalf("failure propagation: %+v", res)
	}
}

func TestOpDispatchTimeout(t *testing.T) {
	srv, _, url := testServer(t)
	c := newWS(t, url, "dev-slow")

	ctx, cancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
	defer cancel()
	resCh := make(chan error, 1)
	go func() {
		_, err := srv.DispatchOp(ctx, "dev-slow", "status", "", nil)
		resCh <- err
	}()
	c.recvOp(t) // consume; never reply
	err := <-resCh
	oe, ok := err.(*OutError)
	if !ok || oe.Code != "op_timeout" {
		t.Fatalf("timeout err: %v", err)
	}
	// late reply after timeout must not panic or leak the channel
	c.send(InMessage{Type: "op_result", OpID: "stale", OK: true, OKMsg: "{}"})
}

func TestOpDisconnectWakesWaiter(t *testing.T) {
	srv, _, url := testServer(t)
	c := newWS(t, url, "dev-die")

	resCh := make(chan error, 1)
	resRes := make(chan *InMessage, 1)
	go func() {
		res, err := srv.DispatchOp(context.Background(), "dev-die", "status", "", nil)
		if err != nil {
			resCh <- err
			return
		}
		resRes <- res
	}()
	c.recvOp(t)
	c.conn.Close(websocket.StatusNormalClosure, "")
	select {
	case err := <-resCh:
		if err != nil {
			if oe, ok := err.(*OutError); !ok || oe.Code != "target_offline" {
				t.Fatalf("disconnect err: %v", err)
			}
		}
	case res := <-resRes:
		if res.Error == nil || res.Error.Code != "target_offline" {
			t.Fatalf("disconnect result: %+v", res)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("waiter not woken on device disconnect")
	}
}

func TestOpMuteFalseField(t *testing.T) {
	srv, _, url := testServer(t)
	c := newWS(t, url, "dev-mute")

	// regression: OpMute previously shared the "muted" json tag with Muted
	// and both were dropped; the device must see "mute":false explicitly.
	go srv.DispatchOp(context.Background(), "dev-mute", "set_mute", "", func() *bool { f := false; return &f }())
	m := c.recvOp(t)
	if m.Op != "set_mute" || m.OpMute == nil || *m.OpMute {
		t.Fatalf("mute arg lost: %+v", m)
	}
}

func TestOpCorrelation(t *testing.T) {
	srv, _, url := testServer(t)
	c := newWS(t, url, "dev-dup")

	type out struct {
		res *InMessage
		err error
	}
	r1 := make(chan out, 1)
	r2 := make(chan out, 1)
	go func() {
		res, err := srv.DispatchOp(context.Background(), "dev-dup", "status", "", nil)
		r1 <- out{res, err}
	}()
	m1 := c.recvOp(t)
	go func() {
		res, err := srv.DispatchOp(context.Background(), "dev-dup", "leave", "", nil)
		r2 <- out{res, err}
	}()
	m2 := c.recvOp(t)

	// reply out of order: second op answered first
	replyOp(t, c, m2, true, `{"ok":true,"left":true,"mode":"linx"}`)
	replyOp(t, c, m1, true, `{"ok":true,"mode":"intercom","room":"一号房","online_count":1}`)

	o2 := <-r2
	if o2.err != nil || !o2.res.OK || o2.res.Mode != "linx" {
		t.Fatalf("op2: %+v %v", o2.res, o2.err)
	}
	o1 := <-r1
	if o1.err != nil || !o1.res.OK || o1.res.Mode != "intercom" {
		t.Fatalf("op1: %+v %v", o1.res, o1.err)
	}
}

func TestNormalizeOpResult(t *testing.T) {
	m := &InMessage{OK: false, OKMsg: `{"ok":false,"error":"not_member","message":"未加入该房间"}`}
	normalizeOpResult(m)
	if m.OK || m.Error == nil || m.Error.Code != "not_member" {
		t.Fatalf("normalize fail path: %+v", m)
	}
	m2 := &InMessage{OK: true, Mode: "intercom"} // softdev shape must pass through
	normalizeOpResult(m2)
	if m2.Mode != "intercom" || m2.Room != nil {
		t.Fatalf("normalize should be no-op for structured: %+v", m2)
	}
}

func TestAdminOpHTTP(t *testing.T) {
	_, _, url := testServer(t)
	c := newWS(t, url, "dev-admin")
	httpURL := strings.Replace(url, "ws", "http", 1)[:strings.LastIndex(strings.Replace(url, "ws", "http", 1), "/ws")]

	go func() {
		m := c.recvOp(t)
		if m.Op != "status" {
			return
		}
		replyOp(t, c, m, true, `{"ok":true,"mode":"linx","muted":false}`)
	}()

	resp, err := http.Post(httpURL+"/admin/op", "application/json",
		strings.NewReader(`{"device_id":"dev-admin","op":"status"}`))
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var body struct {
		OK   bool   `json:"ok"`
		Mode string `json:"mode"`
	}
	json.NewDecoder(resp.Body).Decode(&body)
	if resp.StatusCode != 200 || !body.OK || body.Mode != "linx" {
		t.Fatalf("admin op: %v %+v", resp.StatusCode, body)
	}

	// failure propagates as ok:false in a 200 body (mcpbridge relies on this)
	resp2, _ := http.Post(httpURL+"/admin/op", "application/json",
		strings.NewReader(`{"device_id":"no-device","op":"leave"}`))
	var body2 struct {
		OK    bool      `json:"ok"`
		Error *OutError `json:"error"`
	}
	json.NewDecoder(resp2.Body).Decode(&body2)
	resp2.Body.Close()
	if body2.OK || body2.Error == nil || body2.Error.Code != "target_offline" {
		t.Fatalf("offline admin op: %+v", body2)
	}
}

func TestMCPToolCallEndToEnd(t *testing.T) {
	// mcp routes are bound at Handler() build time: attach before NewServer
	store := newTestStore(t)
	fm := NewFakeMedia()
	srv := NewControlServer(Config{Addr: ":0"}, store, fm)
	srv.AttachMCP("dev-mcp", "")
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)
	wsURL := strings.Replace(ts.URL, "http", "ws", 1) + "/ws"
	c := newWS(t, wsURL, "dev-mcp")
	httpURL := ts.URL

	// open SSE, learn sessionId from endpoint event
	req, _ := http.NewRequest("GET", httpURL+"/mcp", nil)
	r, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Body.Close()
	sc := bufio.NewScanner(r.Body)
	sidCh := make(chan string, 1)
	evCh := make(chan map[string]any, 8)
	go func() {
		var event string
		for sc.Scan() {
			line := sc.Text()
			if strings.HasPrefix(line, "event: ") {
				event = line[7:]
			} else if strings.HasPrefix(line, "data: ") {
				data := line[6:]
				if event == "endpoint" {
					sidCh <- strings.TrimPrefix(data, "/mcp/message?sessionId=")
				} else if event == "message" {
					var m map[string]any
					if json.Unmarshal([]byte(data), &m) == nil {
						evCh <- m
					}
				}
			}
		}
	}()
	var sid string
	select {
	case sid = <-sidCh:
	case <-time.After(3 * time.Second):
		t.Fatal("no endpoint event")
	}

	post := func(body string) {
		resp, err := http.Post(httpURL+"/mcp/message?sessionId="+sid, "application/json", strings.NewReader(body))
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusAccepted {
			t.Fatalf("post status %v", resp.StatusCode)
		}
	}
	post(`{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"intercom.status","arguments":{}}}`)

	// device receives the dispatched op and answers
	m := c.recvOp(t)
	if m.Op != "status" {
		t.Fatalf("op: %+v", m)
	}
	replyOp(t, c, m, true, `{"ok":true,"mode":"intercom","room":"二号房","invite_code":"468911","online_count":1,"muted":false}`)

	select {
	case ev := <-evCh:
		res, _ := ev["result"].(map[string]any)
		content, _ := res["content"].([]any)
		if len(content) == 0 {
			t.Fatalf("mcp result: %v", ev)
		}
		text := content[0].(map[string]any)["text"].(string)
		var parsed map[string]any
		json.Unmarshal([]byte(text), &parsed)
		if parsed["mode"] != "intercom" || res["isError"] == true {
			t.Fatalf("mcp text: %s", text)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no mcp response on SSE")
	}

	// failure path: isError true
	post(`{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"intercom.leave","arguments":{"device_id":"gone"}}}`)
	select {
	case ev := <-evCh:
		res, _ := ev["result"].(map[string]any)
		if res["isError"] != true {
			t.Fatalf("failure not flagged: %v", ev)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no error response on SSE")
	}
}
