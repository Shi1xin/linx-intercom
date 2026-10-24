// mcpbridge connects the platform mcpproxy (X-MCP-Token reverse channel) to the
// intercom control service. The platform dials us over an outbound WSS we
// open; MCP JSON-RPC messages arrive on it and tools/call is translated into
// a server-initiated op on the target device via POST /admin/op.
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"time"

	"github.com/coder/websocket"
)

var tools = []map[string]any{
	{"name": "intercom.join", "description": "让同频设备按六位数字邀请码加入对讲房间",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{
			"invite_code": map[string]any{"type": "string", "description": "六位数字邀请码"},
		}, "required": []string{"invite_code"}}},
	{"name": "intercom.switch", "description": "把设备切换到已加入过的编号房间",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{
			"room_name": map[string]any{"type": "string", "description": "房间名，如 三号房"},
		}, "required": []string{"room_name"}}},
	{"name": "intercom.status", "description": "查询设备的对讲模式、房间与静音状态",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{}}},
	{"name": "intercom.set_mute", "description": "设置对讲麦克风静音",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{
			"muted": map[string]any{"type": "boolean"},
		}, "required": []string{"muted"}}},
	{"name": "intercom.leave", "description": "退出对讲房间回到灵矽对话",
		"inputSchema": map[string]any{"type": "object", "properties": map[string]any{}}},
}

func main() {
	var (
		endpoint = flag.String("endpoint", "", "mcpproxy wss endpoint with token")
		server   = flag.String("server", "http://127.0.0.1:8090", "intercom control server base url")
		target   = flag.String("target", "", "target device id on the control server")
		adminTok = flag.String("admin-token", "", "admin token for /admin/op")
	)
	flag.Parse()
	if *endpoint == "" || *target == "" {
		log.Fatal("need -endpoint and -target")
	}

	for {
		if err := run(*endpoint, *server, *target, *adminTok); err != nil {
			log.Printf("mcpproxy: %v; retry in 5s", err)
		}
		time.Sleep(5 * time.Second)
	}
}

func run(endpoint, server, target, adminTok string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	conn, _, err := websocket.Dial(ctx, endpoint, nil)
	cancel()
	if err != nil {
		return fmt.Errorf("dial: %w", err)
	}
	defer conn.Close(websocket.StatusNormalClosure, "bye")
	conn.SetReadLimit(1 << 20)
	log.Printf("mcpproxy connected")
	for {
		_, data, err := conn.Read(context.Background())
		if err != nil {
			return fmt.Errorf("read: %w", err)
		}
		log.Printf("mcpproxy rx %s", string(data))
		resp, wrapped := handle(data, server, target, adminTok)
		if resp == nil {
			continue
		}
		var out []byte
		if wrapped {
			out, _ = json.Marshal(map[string]any{"type": "mcp", "payload": resp})
		} else {
			out, _ = json.Marshal(resp)
		}
		if err := conn.Write(context.Background(), websocket.MessageText, out); err != nil {
			return fmt.Errorf("write: %w", err)
		}
		log.Printf("mcpproxy tx %s", string(out))
	}
}

type rpcReq struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params"`
}

func handle(data []byte, server, target, adminTok string) (any, bool) {
	// accept raw JSON-RPC or the device-mcp wrapper {"type":"mcp","payload":{..}}
	payload := data
	var wrap struct {
		Type    string          `json:"type"`
		Payload json.RawMessage `json:"payload"`
	}
	wrapped := false
	if json.Unmarshal(data, &wrap) == nil && len(wrap.Payload) > 0 {
		payload = wrap.Payload
		wrapped = true
	}
	var req rpcReq
	if json.Unmarshal(payload, &req) != nil || req.Method == "" {
		return nil, wrapped
	}
	reply := func(result any, rpcErr any) (map[string]any, bool) {
		r := map[string]any{"jsonrpc": "2.0", "id": json.RawMessage(req.ID)}
		if len(req.ID) == 0 {
			r["id"] = json.RawMessage("0")
		}
		if rpcErr != nil {
			r["error"] = rpcErr
		} else {
			r["result"] = result
		}
		return r, wrapped
	}
	switch req.Method {
	case "initialize":
		return reply(map[string]any{
			"protocolVersion": "2024-11-05",
			"capabilities":    map[string]any{"tools": map[string]any{}},
			"serverInfo":      map[string]any{"name": "linx-intercom-bridge", "version": "0.1.0"},
		}, nil)
	case "tools/list", "notifications/initialized":
		if req.Method == "tools/list" {
			return reply(map[string]any{"tools": tools}, nil)
		}
		return nil, wrapped
	case "tools/call":
		var p struct {
			Name      string         `json:"name"`
			Arguments map[string]any `json:"arguments"`
		}
		_ = json.Unmarshal(req.Params, &p)
		text, isErr := callTool(server, target, adminTok, p.Name, p.Arguments)
		return reply(map[string]any{
			"content": []map[string]any{{"type": "text", "text": text}},
			"isError": isErr,
		}, nil)
	default:
		if len(req.ID) == 0 {
			return nil, wrapped
		}
		return reply(nil, map[string]any{"code": -32601, "message": "unsupported " + req.Method})
	}
}

func callTool(server, target, adminTok, name string, args map[string]any) (string, bool) {
	var op, arg string
	var muted any
	switch name {
	case "intercom.join":
		code, _ := args["invite_code"].(string)
		if len(code) != 6 {
			return "invite_code 需要六位数字字符串", true
		}
		op, arg = "join", code
	case "intercom.switch":
		n, _ := args["room_name"].(string)
		if n == "" {
			return "room_name required", true
		}
		op, arg = "switch", n
	case "intercom.status":
		op = "status"
	case "intercom.set_mute":
		m, ok := args["muted"].(bool)
		if !ok {
			return "muted must be boolean", true
		}
		muted = m
		op = "set_mute"
	case "intercom.leave":
		op = "leave"
	default:
		return "unknown tool " + name, true
	}
	body, _ := json.Marshal(map[string]any{
		"device_id": target, "op": op, "arg": arg, "muted": muted,
	})
	req, _ := http.NewRequest("POST", server+"/admin/op", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	if adminTok != "" {
		req.Header.Set("X-Admin-Token", adminTok)
	}
	res, err := opClient.Do(req)
	if err != nil {
		return "server op failed: " + err.Error(), true
	}
	defer res.Body.Close()
	out, _ := io.ReadAll(res.Body)
	log.Printf("op %s(%s) -> %s", op, arg, string(out))
	// admin/op answers HTTP 200 even on op failure; judge by the body's ok field
	var resObj struct {
		OK      bool            `json:"ok"`
		Error   json.RawMessage `json:"error"`
		Message string          `json:"message"`
	}
	if res.StatusCode != 200 {
		return string(out), true
	}
	if json.Unmarshal(out, &resObj) != nil {
		return string(out), true  /* unparsable body = upstream error page */
	}
	if !resObj.OK {
		return string(out), true
	}
	return string(out), false
}

var opClient = &http.Client{Timeout: 35 * time.Second}
