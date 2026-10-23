package server

// Control protocol message types. See docs/control-protocol.md.

type InMessage struct {
	Type      string          `json:"type"`
	RequestID string          `json:"request_id,omitempty"`
	DeviceID  string          `json:"device_id,omitempty"`
	Token     string          `json:"token,omitempty"`
	Target    *JoinTarget     `json:"target,omitempty"`
	SDP       string          `json:"sdp,omitempty"`
	Candidate *IceCandidate   `json:"candidate,omitempty"`
	Completed bool            `json:"completed,omitempty"`
	Muted     *bool           `json:"muted,omitempty"`
}

type JoinTarget struct {
	Kind     string `json:"kind"` // nfc | device | invite | room_name
	EntryID  string `json:"entry_id,omitempty"`
	DeviceID string `json:"device_id,omitempty"`
	Code     string `json:"code,omitempty"`
	Name     string `json:"name,omitempty"`
}

type IceCandidate struct {
	Candidate     string `json:"candidate"`
	SDPMid        string `json:"sdpMid,omitempty"`
	SDPMLineIndex int    `json:"sdpMLineIndex,omitempty"`
}

type RoomInfo struct {
	ID          int64  `json:"id"`
	Name        string `json:"name"`
	InviteCode  string `json:"invite_code"`
	OnlineCount int    `json:"online_count"`
}

type OutMessage struct {
	Type        string      `json:"type"`
	RequestID   string      `json:"request_id,omitempty"`
	OK          bool        `json:"ok,omitempty"`
	Muted       bool        `json:"muted,omitempty"`
	ResumeRoom  *RoomInfo   `json:"resume_room,omitempty"`
	Room        *RoomInfo   `json:"room,omitempty"`
	NeedMedia   bool        `json:"need_media,omitempty"`
	Jsep        string      `json:"jsep,omitempty"`
	SDP         string      `json:"sdp,omitempty"`
	Candidate   *IceCandidate `json:"candidate,omitempty"`
	Completed   bool        `json:"completed,omitempty"`
	Name        string      `json:"name,omitempty"`
	OnlineCount int         `json:"online_count,omitempty"`
	InviteCode  string      `json:"invite_code,omitempty"`
	InviteDigits string     `json:"invite_digits,omitempty"`
	Members     []string    `json:"members,omitempty"`
	Event       string      `json:"event,omitempty"`
	DeviceID    string      `json:"device_id,omitempty"`
	Error       *OutError   `json:"error,omitempty"`
}

type OutError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

func (e *OutError) Error() string { return e.Code + ": " + e.Message }

func errOut(reqID, code, msg string) *OutMessage {
	return &OutMessage{Type: "error", RequestID: reqID, Error: &OutError{Code: code, Message: msg}}
}
