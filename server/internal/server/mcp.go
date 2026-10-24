package server

// Cloud MCP adapter: MCP over SSE per docs/xrobot/mcp/software-mcp.
//
//	GET  /mcp          -> SSE stream; first event "endpoint" carries the POST url
//	POST /mcp/message  -> JSON-RPC requests (sessionId query param)
//
// The platform injects X-Linx-Device-Id / X-Linx-Agent-Id / X-Linx-Session-Id.
// intercom.* tools dispatch ops to the target device's control session; when
// the tool arg omits device_id, the X-Linx-Device-Id header (or the configured
// default target) selects the device.

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"sync"
	"time"
)

type mcpSession struct {
	out chan []byte
}

type MCPAdapter struct {
	srv           *ControlServer
	defaultTarget string // fallback target device id when header+arg absent
	token         string // when set, /mcp requires Authorization: Bearer or ?token=

	mu       sync.Mutex
	sessions map[string]*mcpSession
	seq      int64
}

func NewMCPAdapter(srv *ControlServer, defaultTarget, token string) *MCPAdapter {
	return &MCPAdapter{srv: srv, defaultTarget: defaultTarget, token: token, sessions: map[string]*mcpSession{}}
}

func (a *MCPAdapter) authorized(r *http.Request) bool {
	if a.token == "" {
		return true
	}
	return r.URL.Query().Get("token") == a.token ||
		r.Header.Get("Authorization") == "Bearer "+a.token
}

var cloudTools = []map[string]any{
	{"name": "intercom.join", "description": "让目标同频设备按六位数字邀请码加入对讲房间",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{
			"invite_code": map[string]any{"type": "string", "description": "六位数字邀请码"},
			"device_id":   map[string]any{"type": "string", "description": "目标设备 id，缺省取调用方设备"},
		}, "required": []string{"invite_code"}}},
	{"name": "intercom.switch", "description": "把目标设备切换到已加入过的编号房间",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{
			"room_name": map[string]any{"type": "string", "description": "房间名，如 三号房"},
			"device_id": map[string]any{"type": "string"},
		}, "required": []string{"room_name"}}},
	{"name": "intercom.status", "description": "查询目标设备的对讲模式、房间与静音状态",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{
			"device_id": map[string]any{"type": "string"},
		}}},
	{"name": "intercom.set_mute", "description": "设置目标设备对讲麦克风静音",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{
			"muted":     map[string]any{"type": "boolean"},
			"device_id": map[string]any{"type": "string"},
		}, "required": []string{"muted"}}},
	{"name": "intercom.leave", "description": "让目标设备退出对讲房间回到灵矽对话",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{
			"device_id": map[string]any{"type": "string"},
		}}},
}

func (a *MCPAdapter) Register(mux *http.ServeMux) {
	mux.HandleFunc("/mcp", a.handleSSE)             // SSE stream (GET)
	mux.HandleFunc("/mcp/message", a.handleMessage) // JSON-RPC POST
}

func (a *MCPAdapter) handleSSE(w http.ResponseWriter, r *http.Request) {
	if !a.authorized(r) {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	if r.Method != http.MethodGet {
		http.Error(w, "method", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	fl, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "no sse", http.StatusInternalServerError)
		return
	}
	a.mu.Lock()
	a.seq++
	id := fmt.Sprintf("mcp%d", a.seq)
	s := &mcpSession{out: make(chan []byte, 64)}
	a.sessions[id] = s
	a.mu.Unlock()
	defer func() {
		a.mu.Lock()
		delete(a.sessions, id)
		a.mu.Unlock()
	}()

	fmt.Fprintf(w, "event: endpoint\ndata: /mcp/message?sessionId=%s\n\n", id)
	fl.Flush()
	tick := time.NewTicker(30 * time.Second)
	defer tick.Stop()
	for {
		select {
		case data := <-s.out:
			fmt.Fprintf(w, "event: message\ndata: %s\n\n", data)
			fl.Flush()
		case <-r.Context().Done():
			return
		case <-tick.C:
			fmt.Fprintf(w, ": keepalive\n\n")
			fl.Flush()
		}
	}
}

func (a *MCPAdapter) handleMessage(w http.ResponseWriter, r *http.Request) {
	if !a.authorized(r) {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method", http.StatusMethodNotAllowed)
		return
	}
	sid := r.URL.Query().Get("sessionId")
	a.mu.Lock()
	s := a.sessions[sid]
	a.mu.Unlock()
	if s == nil {
		http.Error(w, "unknown session", http.StatusNotFound)
		return
	}
	var req struct {
		JSONRPC string          `json:"jsonrpc"`
		ID      json.RawMessage `json:"id"`
		Method  string          `json:"method"`
		Params  json.RawMessage `json:"params"`
	}
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "bad json", http.StatusBadRequest)
		return
	}
	srcDevice := r.Header.Get("X-Linx-Device-Id")
	srcAgent := r.Header.Get("X-Linx-Agent-Id")
	w.WriteHeader(http.StatusAccepted)
	w.Write([]byte("Accepted"))
	if fl, ok := w.(http.Flusher); ok {
		fl.Flush() // release the POST immediately; result rides the SSE stream
	}
	resp := a.dispatch(&req, srcDevice, srcAgent)
	if resp != nil {
		data, _ := json.Marshal(resp)
		select {
		case s.out <- data:
		default:
			log.Printf("mcp session %s out full, drop", sid)
		}
	}
}

type rpcResp struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Result  any             `json:"result,omitempty"`
	Error   any             `json:"error,omitempty"`
}

func (a *MCPAdapter) dispatch(req *struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params"`
}, srcDevice, srcAgent string) *rpcResp {
	reply := &rpcResp{JSONRPC: "2.0", ID: req.ID}
	fail := func(code int, msg string) *rpcResp {
		reply.Error = map[string]any{"code": code, "message": msg}
		return reply
	}
	switch req.Method {
	case "initialize":
		reply.Result = map[string]any{
			"protocolVersion": "2024-11-05",
			"capabilities":    map[string]any{"tools": map[string]any{}},
			"serverInfo":      map[string]any{"name": "linx-intercom", "version": "0.1.0"},
		}
		return reply
	case "tools/list":
		reply.Result = map[string]any{"tools": cloudTools}
		return reply
	case "tools/call":
		if len(req.ID) == 0 {
			return nil // notification form: never execute side effects
		}
		return a.callTool(req, srcDevice)
	default:
		if len(req.ID) == 0 {
			return nil // notification
		}
		return fail(-32601, "unsupported method "+req.Method)
	}
}

func (a *MCPAdapter) callTool(req *struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params"`
}, srcDevice string) *rpcResp {
	var p struct {
		Name      string          `json:"name"`
		Arguments json.RawMessage `json:"arguments"`
	}
	_ = json.Unmarshal(req.Params, &p)
	var args map[string]any
	_ = json.Unmarshal(p.Arguments, &args)
	target, _ := args["device_id"].(string)
	if target == "" {
		target = srcDevice // platform header: the device whose session called
	}
	if target == "" {
		target = a.defaultTarget
	}
	toolErr := func(msg string) *rpcResp {
		return &rpcResp{JSONRPC: "2.0", ID: req.ID, Result: map[string]any{
			"content": []map[string]any{{"type": "text", "text": msg}},
			"isError": true}}
	}
	toolOK := func(res *InMessage) *rpcResp {
		text, _ := json.Marshal(map[string]any{
			"ok": res.OK, "mode": res.Mode, "room": res.Room,
			"muted": res.Muted, "message": res.OKMsg, "error": res.Error})
		return &rpcResp{JSONRPC: "2.0", ID: req.ID, Result: map[string]any{
			"content": []map[string]any{{"type": "text", "text": string(text)}},
			"isError": !res.OK}}
	}
	if target == "" {
		return toolErr("无法确定目标设备：请提供 device_id 或从已关联设备会话调用")
	}
	var op, arg string
	var muted *bool
	switch p.Name {
	case "intercom.join":
		c, _ := args["invite_code"].(string)
		if len(c) != 6 {
			return toolErr("invite_code 需要六位数字字符串")
		}
		op, arg = "join", c
	case "intercom.switch":
		n, _ := args["room_name"].(string)
		if n == "" {
			return toolErr("room_name required")
		}
		op, arg = "switch", n
	case "intercom.status":
		op = "status"
	case "intercom.set_mute":
		m, ok := args["muted"].(bool)
		if !ok {
			return toolErr("muted must be boolean")
		}
		muted = &m
		op = "set_mute"
	case "intercom.leave":
		op = "leave"
	default:
		return toolErr("unknown tool " + p.Name)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Second)
	defer cancel()
	res, err := a.srv.DispatchOp(ctx, target, op, arg, muted)
	if err != nil {
		msg := err.Error()
		if e, ok2 := err.(*OutError); ok2 {
			msg = e.Code + ": " + e.Message
		}
		return toolErr(msg)
	}
	return toolOK(res)
}
