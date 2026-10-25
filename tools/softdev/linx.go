package main

// Linx platform WebSocket: device side of xrobo-io protocol with MCP server.
// See .local/platform-assessment/docs-websocket.json / docs-hardware-mcp.json.

import (
	"context"
	"encoding/json"
	"log"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
)

type linxConn struct {
	conn      *websocket.Conn
	sendMu    sync.Mutex
	sessionID atomic.Value
	dev       *softDevice
	advMCP    bool // advertise mcp feature in hello
}

type mcpPayload struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id,omitempty"`
	Method  string          `json:"method,omitempty"`
	Params  json.RawMessage `json:"params,omitempty"`
}

type toolCallParams struct {
	Name      string          `json:"name"`
	Arguments json.RawMessage `json:"arguments"`
}

var deviceTools = []map[string]any{
	{
		"name":        "self.intercom.join",
		"description": "按六位数字邀请码让本设备加入同频对讲房间。成功后设备进入对讲模式。",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{
			"invite_code": map[string]any{"type": "string", "description": "六位数字邀请码，逐位数字组合成字符串"},
		}, "required": []string{"invite_code"}},
	},
	{
		"name":        "self.intercom.switch",
		"description": "把本设备切换到已加入过的另一个编号房间。",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{
			"room_name": map[string]any{"type": "string", "description": "房间名，例如 三号房"},
		}, "required": []string{"room_name"}},
	},
	{
		"name":        "self.intercom.create",
		"description": "新建一个同频对讲房间并让本设备进入。成功后把房间名和邀请码告诉用户，邀请码按 invite_spoken 字段逐位读出，方便用户转告队友。",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{}},
	},
	{
		"name":        "self.intercom.status",
		"description": "查询本设备当前模式、房间、人数、邀请码与静音状态。",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{}},
	},
	{
		"name":        "self.intercom.set_mute",
		"description": "设置本设备对讲麦克风静音状态。",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{
			"muted": map[string]any{"type": "boolean", "description": "true 静音，false 取消"},
		}, "required": []string{"muted"}},
	},
	{
		"name":        "self.intercom.leave",
		"description": "退出当前对讲房间，设备回到灵矽对话模式。",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{}},
	},
}

func dialLinx(url, token, devID, clientID string, advMCP bool) (*linxConn, error) {
	hdr := http.Header{}
	hdr.Set("Authorization", "Bearer "+token)
	hdr.Set("Device-Id", devID)
	hdr.Set("Client-Id", clientID)
	hdr.Set("Protocol-Version", "1")
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	// coder/websocket dials TCP/TLS directly; ambient http_proxy env vars do not apply.
	conn, _, err := websocket.Dial(ctx, url, &websocket.DialOptions{HTTPHeader: hdr})
	if err != nil {
		return nil, err
	}
	conn.SetReadLimit(4 << 20)
	l := &linxConn{conn: conn, advMCP: advMCP}
	return l, nil
}

func (l *linxConn) close() {
	_ = l.conn.Close(websocket.StatusNormalClosure, "")
}

func (l *linxConn) sendJSON(v any) {
	data, _ := json.Marshal(v)
	l.dev.trace("linx-tx", json.RawMessage(data))
	l.sendMu.Lock()
	defer l.sendMu.Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	if err := l.conn.Write(ctx, websocket.MessageText, data); err != nil {
		log.Printf("linx send: %v", err)
	}
}

func (l *linxConn) hello() {
	l.sendJSON(map[string]any{
		"type": "hello", "version": 1, "transport": "websocket",
		"features":    map[string]any{"mcp": l.advMCP},
		"audio_params": map[string]any{"format": "opus", "sample_rate": 16000,
			"channels": 1, "frame_duration": 20},
	})
}

func (l *linxConn) sendDetectText(text string) {
	sid, _ := l.sessionID.Load().(string)
	l.sendJSON(map[string]any{
		"type": "listen", "state": "detect", "text": text, "session_id": sid})
}

func (l *linxConn) mcpReply(id json.RawMessage, result any) {
	l.sendJSON(map[string]any{"type": "mcp", "payload": map[string]any{
		"jsonrpc": "2.0", "id": json.RawMessage(id), "result": result}})
}

func (l *linxConn) mcpError(id json.RawMessage, code int, msg string) {
	l.sendJSON(map[string]any{"type": "mcp", "payload": map[string]any{
		"jsonrpc": "2.0", "id": json.RawMessage(id),
		"error": map[string]any{"code": code, "message": msg}}})
}

func (l *linxConn) mcpToolResult(id json.RawMessage, result map[string]any, callErr error) {
	isErr := callErr != nil
	text, _ := json.Marshal(map[string]any{
		"result": result, "error": errString(callErr), "mode": l.dev.modeSnapshot()})
	l.mcpReply(id, map[string]any{
		"content": []map[string]any{{"type": "text", "text": string(text)}},
		"isError": isErr,
	})
}

func errString(e error) any {
	if e == nil {
		return nil
	}
	return e.Error()
}

// handleMCP processes one platform mcp message; returns tool-call info when a
// tools/call was executed.
func (l *linxConn) handleMCP(raw json.RawMessage) (string, bool) {
	var p mcpPayload
	if json.Unmarshal(raw, &p) != nil {
		return "", false
	}
	switch p.Method {
	case "initialize":
		l.mcpReply(p.ID, map[string]any{
			"protocolVersion": "2024-11-05",
			"capabilities":    map[string]any{"tools": map[string]any{}},
			"serverInfo":      map[string]any{"name": "linx-intercom-softdev", "version": "0.2.0"},
		})
	case "tools/list":
		l.mcpReply(p.ID, map[string]any{"tools": deviceTools, "nextCursor": nil})
	case "tools/call":
		var params toolCallParams
		if json.Unmarshal(p.Params, &params) != nil {
			l.mcpError(p.ID, -32602, "bad tools/call params")
			return "", false
		}
		var args map[string]any
		if len(params.Arguments) > 0 && params.Arguments[0] == '{' {
			_ = json.Unmarshal(params.Arguments, &args)
		}
		log.Printf("tools/call %s args=%s", params.Name, string(params.Arguments))
		result, err := l.dev.execOp(params.Name, args)
		l.mcpToolResult(p.ID, result, err)
		return params.Name, true
	default:
		if len(p.ID) > 0 {
			l.mcpError(p.ID, -32601, "unsupported method "+p.Method)
		}
	}
	return "", false
}

// run is the platform read loop; returns when conn dies or ctx ends.
// onTTS receives every tts state ("start"/"sentence_start"/"stop") so script
// drivers can pace prompts to reply boundaries.
func (l *linxConn) run(ctx context.Context, onToolsListed func(), onToolCall func(name string), onTTS func(state string)) error {
	for {
		mt, data, err := l.conn.Read(ctx)
		if err != nil {
			return err
		}
		if mt == websocket.MessageBinary {
			l.dev.audioFrames.Add(1)
			l.dev.trace("linx-rx-audio", map[string]any{"bytes": len(data),
				"mode": l.dev.modeSnapshot()})
			continue
		}
		var m struct {
			Type    string          `json:"type"`
			Session string          `json:"session_id"`
			Payload json.RawMessage `json:"payload"`
			State   string          `json:"state"`
		}
		if json.Unmarshal(data, &m) != nil {
			continue
		}
		l.dev.trace("linx-rx", json.RawMessage(data))
		switch m.Type {
		case "hello":
			l.sessionID.Store(m.Session)
			log.Printf("linx hello ok session=%s", m.Session)
		case "mcp":
			var pm mcpPayload
			_ = json.Unmarshal(m.Payload, &pm)
			name, called := l.handleMCP(m.Payload)
			log.Printf("mcp %s", json.RawMessage(m.Payload))
			if pm.Method == "tools/list" && onToolsListed != nil {
				onToolsListed()
			}
			if called && onToolCall != nil {
				onToolCall(name)
			}
		case "tts":
			if m.State == "stop" {
				l.dev.trace("linx-tts-stop", nil)
			}
			if onTTS != nil {
				onTTS(m.State)
			}
		}
	}
}
