package main

// Unified device-side executor: platform MCP calls, stdin ops and (later) NFC /
// cloud-dispatched ops all funnel through execOp against the control channel.

import (
	"fmt"
	"sync"
	"sync/atomic"
	"time"
)

type softDevice struct {
	ctrl *ctrlClient

	opMu     sync.Mutex // serialize room ops
	mu       sync.Mutex
	mode     string // "linx" | "intercom"
	room     *RoomInfo
	muted    bool
	mediaSess *mediaSession

	deviceID string
	token    string
	turnAddr string
	oggPath  string

	traceMu  sync.Mutex
	traceLog []traceEntry
	started  time.Time

	audioFrames atomic.Int64
}

type traceEntry struct {
	Seconds float64 `json:"seconds"`
	Dir     string  `json:"dir"`
	Message any     `json:"message"`
}

func (d *softDevice) trace(dir string, m any) {
	d.traceMu.Lock()
	d.traceLog = append(d.traceLog, traceEntry{
		Seconds: float64(time.Since(d.started).Milliseconds()) / 1000,
		Dir:     dir, Message: m})
	d.traceMu.Unlock()
}

func (d *softDevice) setMedia(m *mediaSession) {
	d.mu.Lock()
	old := d.mediaSess
	d.mediaSess = m
	d.mu.Unlock()
	if old != nil {
		old.teardown()
	}
}

func (d *softDevice) currentMedia() *mediaSession {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.mediaSess
}

// callbacks invoked from ctrl readLoop
func (d *softDevice) onMediaAnswer(sdp string) {
	if m := d.currentMedia(); m != nil {
		m.setAnswer(sdp)
	}
}

func (d *softDevice) onRemoteCandidate(c iceCand) {
	if m := d.currentMedia(); m != nil {
		m.addCandidate(c)
	}
}

func (d *softDevice) onNotify(event, peerID string) {
	switch event {
	case "media_lost":
		d.setMedia(nil)
		d.mu.Lock()
		d.room = nil
		d.mu.Unlock()
	case "summon":
		// a peer device tagged us; join its room by invite code
	}
}

// execOp runs one tool call. Returns the JSON-able result payload.
func (d *softDevice) execOp(tool string, args map[string]any) (map[string]any, error) {
	d.opMu.Lock()
	defer d.opMu.Unlock()
	switch tool {
	case "self.intercom.join":
		code, _ := args["invite_code"].(string)
		if len(code) != 6 {
			return nil, fmt.Errorf("invite_code must be a 6-digit string")
		}
		return d.opJoin(map[string]string{"kind": "invite", "code": code})
	case "self.intercom.switch":
		name, _ := args["room_name"].(string)
		if name == "" {
			return nil, fmt.Errorf("room_name required")
		}
		return d.opJoin(map[string]string{"kind": "room_name", "name": name})
	case "self.intercom.status":
		return d.opStatus()
	case "self.intercom.set_mute":
		muted, ok := args["muted"].(bool)
		if !ok {
			return nil, fmt.Errorf("muted must be boolean")
		}
		return d.opMute(muted)
	case "self.intercom.leave":
		return d.opLeave()
	default:
		return nil, fmt.Errorf("unknown tool %s", tool)
	}
}

func (d *softDevice) opJoin(target map[string]string) (map[string]any, error) {
	j, err := d.ctrl.join(target)
	if err != nil {
		return nil, err
	}
	if !j.OK {
		code, msg := "join_failed", ""
		if j.Error != nil {
			code, msg = j.Error.Code, j.Error.Message
		}
		return nil, fmt.Errorf("%s: %s", code, msg)
	}
	mediaState := "reused"
	if j.NeedMedia {
		m, err := newMedia(d, d.turnAddr)
		if err != nil {
			return nil, fmt.Errorf("media init: %w", err)
		}
		d.setMedia(m)
		if err := m.offer(); err != nil {
			d.setMedia(nil)
			return nil, fmt.Errorf("media offer: %w", err)
		}
		if !m.waitConnected(15 * time.Second) {
			d.setMedia(nil)
			return nil, fmt.Errorf("media_failed: webrtc connect timeout")
		}
		mediaState = "connected"
		if d.oggPath != "" {
			m.streamOgg(d.oggPath)
		}
	}
	cf, err := d.ctrl.confirm()
	if err != nil {
		return nil, err
	}
	if !cf.OK {
		return nil, fmt.Errorf("confirm_failed: %+v", cf.Error)
	}
	d.mu.Lock()
	d.mode = "intercom"
	d.room = cf.Room
	d.mu.Unlock()
	return map[string]any{
		"ok": true, "mode": "intercom", "media": mediaState,
		"room": cf.Room.Name, "invite_code": cf.Room.InviteCode,
		"online_count": cf.Room.OnlineCount,
	}, nil
}

func (d *softDevice) opStatus() (map[string]any, error) {
	d.mu.Lock()
	mode, room, muted := d.mode, d.room, d.muted
	hasMedia := d.mediaSess != nil
	d.mu.Unlock()
	out := map[string]any{"mode": mode, "muted": muted, "media": hasMedia}
	if mode == "intercom" && room != nil {
		ri, err := d.ctrl.roomInfo()
		if err != nil {
			return out, nil // still report local state
		}
		if ri.OK {
			out["room"] = ri.Name
			out["invite_code"] = ri.InviteCode
			out["invite_digits"] = ri.InviteDigits
			out["online_count"] = ri.OnlineCount
			out["members"] = ri.Members
		}
	}
	return out, nil
}

func (d *softDevice) opMute(muted bool) (map[string]any, error) {
	m, err := d.ctrl.setMute(muted)
	if err != nil {
		return nil, err
	}
	if !m.OK {
		return nil, fmt.Errorf("mute failed: %+v", m.Error)
	}
	d.mu.Lock()
	d.muted = muted
	d.mu.Unlock()
	return map[string]any{"ok": true, "muted": muted}, nil
}

func (d *softDevice) opLeave() (map[string]any, error) {
	l, err := d.ctrl.leave()
	if err != nil {
		return nil, err
	}
	if !l.OK {
		return nil, fmt.Errorf("leave failed: %+v", l.Error)
	}
	d.setMedia(nil)
	d.mu.Lock()
	d.mode = "linx"
	d.room = nil
	d.mu.Unlock()
	return map[string]any{"ok": true, "left": true, "mode": "linx"}, nil
}
