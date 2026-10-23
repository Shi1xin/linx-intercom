package main

// Control-channel client: speaks docs/control-protocol.md over ws JSON.
// One request in flight per request_id; responses matched on (type, request_id).

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
)

type iceCand struct {
	Candidate     string `json:"candidate"`
	SDPMid        string `json:"sdpMid"`
	SDPMLineIndex int    `json:"sdpMLineIndex"`
}

type RoomInfo struct {
	ID          int64  `json:"id"`
	Name        string `json:"name"`
	InviteCode  string `json:"invite_code"`
	OnlineCount int    `json:"online_count"`
}

type ctrlMsg struct {
	Type      string `json:"type"`
	RequestID string `json:"request_id,omitempty"`
	DeviceID  string `json:"device_id,omitempty"`
	Token     string `json:"token,omitempty"`
	OK        bool   `json:"ok,omitempty"`
	NeedMedia bool   `json:"need_media,omitempty"`
	Jsep      string `json:"jsep,omitempty"`
	SDP       string `json:"sdp,omitempty"`
	Target    map[string]string `json:"target,omitempty"`
	ResumeRoom *RoomInfo `json:"resume_room,omitempty"`
	Room      *RoomInfo `json:"room,omitempty"`
	Candidate *iceCand  `json:"candidate,omitempty"`
	Completed bool      `json:"completed,omitempty"`
	Error     *struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error,omitempty"`
	Name         string   `json:"name,omitempty"`
	InviteCode   string   `json:"invite_code,omitempty"`
	InviteDigits string   `json:"invite_digits,omitempty"`
	OnlineCount  int      `json:"online_count,omitempty"`
	Members      []string `json:"members,omitempty"`
	Event        string   `json:"event,omitempty"`
	Muted        *bool    `json:"muted,omitempty"`
}

type ctrlClient struct {
	conn   *websocket.Conn
	sendMu sync.Mutex
	seq    atomic.Int64
	recvCh chan ctrlMsg
	dev    *softDevice
}

func dialCtrl(url string) (*ctrlClient, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, url, nil)
	if err != nil {
		return nil, err
	}
	c := &ctrlClient{conn: conn, recvCh: make(chan ctrlMsg, 128)}
	go c.readLoop()
	return c, nil
}

func (c *ctrlClient) close() {
	_ = c.conn.Close(websocket.StatusNormalClosure, "")
}

func (c *ctrlClient) send(m ctrlMsg) string {
	c.seq.Add(1)
	if m.RequestID == "" {
		m.RequestID = fmt.Sprintf("r%d", c.seq.Load())
	}
	data, _ := json.Marshal(m)
	c.sendMu.Lock()
	defer c.sendMu.Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	if err := c.conn.Write(ctx, websocket.MessageText, data); err != nil {
		log.Printf("ctrl send: %v", err)
	}
	c.dev.trace("ctrl-tx", m)
	return m.RequestID
}

func (c *ctrlClient) readLoop() {
	for {
		_, data, err := c.conn.Read(context.Background())
		if err != nil {
			close(c.recvCh)
			return
		}
		var m ctrlMsg
		if json.Unmarshal(data, &m) != nil {
			continue
		}
		c.dev.trace("ctrl-rx", m)
		switch m.Type {
		case "sdp":
			if m.Jsep == "answer" {
				c.dev.onMediaAnswer(m.SDP)
			}
		case "ice":
			if !m.Completed && m.Candidate != nil {
				c.dev.onRemoteCandidate(*m.Candidate)
			}
		case "notify":
			c.dev.onNotify(m.Event, m.DeviceID)
		}
		c.recvCh <- m
	}
}

// waitResp waits for a message of the given type carrying wantID.
// Messages with a different request_id are left in the channel only if they
// arrive before ours; unsolicited notifies are consumed by readLoop hooks but
// also land here, so non-matching frames are discarded.
func (c *ctrlClient) waitResp(wantType, wantID string, timeout time.Duration) (ctrlMsg, error) {
	t := time.NewTimer(timeout)
	defer t.Stop()
	for {
		select {
		case m, ok := <-c.recvCh:
			if !ok {
				return m, fmt.Errorf("ctrl conn closed waiting %s", wantType)
			}
			if m.Type == wantType && (wantID == "" || m.RequestID == wantID) {
				return m, nil
			}
		case <-t.C:
			return ctrlMsg{}, fmt.Errorf("timeout waiting %s", wantType)
		}
	}
}

func (c *ctrlClient) hello(devID, token string) (ctrlMsg, error) {
	id := c.send(ctrlMsg{Type: "hello", DeviceID: devID, Token: token})
	m, err := c.waitResp("hello_result", id, 10*time.Second)
	if err != nil {
		return m, err
	}
	if !m.OK {
		return m, fmt.Errorf("hello rejected: %+v", m.Error)
	}
	return m, nil
}

func (c *ctrlClient) join(target map[string]string) (ctrlMsg, error) {
	id := c.send(ctrlMsg{Type: "join", Target: target})
	return c.waitResp("join_result", id, 15*time.Second)
}

func (c *ctrlClient) confirm() (ctrlMsg, error) {
	id := c.send(ctrlMsg{Type: "confirm"})
	return c.waitResp("confirm_result", id, 10*time.Second)
}

func (c *ctrlClient) roomInfo() (ctrlMsg, error) {
	id := c.send(ctrlMsg{Type: "room_info"})
	return c.waitResp("room_info_result", id, 8*time.Second)
}

func (c *ctrlClient) setMute(muted bool) (ctrlMsg, error) {
	id := c.send(ctrlMsg{Type: "set_mute", Muted: &muted})
	return c.waitResp("mute_result", id, 8*time.Second)
}

func (c *ctrlClient) leave() (ctrlMsg, error) {
	id := c.send(ctrlMsg{Type: "leave"})
	return c.waitResp("leave_result", id, 8*time.Second)
}
