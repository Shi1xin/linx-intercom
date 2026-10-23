// wsprobe: headless control-protocol + WebRTC probe client.
// Joins a room via the control service, streams a real Opus ogg file to
// Janus AudioBridge, counts inbound mixed packets and can record the
// downlink to an ogg file for offline analysis.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
	"github.com/pion/webrtc/v4"
	"github.com/pion/webrtc/v4/pkg/media"
	"github.com/pion/webrtc/v4/pkg/media/oggreader"
	"github.com/pion/webrtc/v4/pkg/media/oggwriter"
)

type iceCand struct {
	Candidate     string `json:"candidate"`
	SDPMid        string `json:"sdpMid"`
	SDPMLineIndex int    `json:"sdpMLineIndex"`
}

type msg struct {
	Type      string          `json:"type"`
	RequestID string          `json:"request_id,omitempty"`
	DeviceID  string          `json:"device_id,omitempty"`
	OK        bool            `json:"ok,omitempty"`
	NeedMedia bool            `json:"need_media,omitempty"`
	Jsep      string          `json:"jsep,omitempty"`
	SDP       string          `json:"sdp,omitempty"`
	Target    map[string]string `json:"target,omitempty"`
	ResumeRoom json.RawMessage `json:"resume_room,omitempty"`
	Room      struct {
		ID          int64  `json:"id"`
		Name        string `json:"name"`
		InviteCode  string `json:"invite_code"`
		OnlineCount int    `json:"online_count"`
	} `json:"room,omitempty"`
	Candidate   *iceCand `json:"candidate,omitempty"`
	Completed   bool     `json:"completed,omitempty"`
	Error       *struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error,omitempty"`
	Name         string `json:"name,omitempty"`
	InviteCode   string `json:"invite_code,omitempty"`
	InviteDigits string `json:"invite_digits,omitempty"`
	OnlineCount  int    `json:"online_count,omitempty"`
	Event        string `json:"event,omitempty"`
}

type client struct {
	conn   *websocket.Conn
	sendMu sync.Mutex
	seq    int
	recvCh chan msg
	pc     *webrtc.PeerConnection
	track  *webrtc.TrackLocalStaticSample
	rxPkts atomic.Int64
	rxBytes atomic.Int64
	txPkts  atomic.Int64
	connected atomic.Bool
	remoteDesc atomic.Bool
	pendingICE []webrtc.ICECandidateInit
	iceMu      sync.Mutex
	rec *oggwriter.OggWriter
}

func (c *client) addRemoteCandidate(cd webrtc.ICECandidateInit) {
	c.iceMu.Lock()
	defer c.iceMu.Unlock()
	if !c.remoteDesc.Load() {
		c.pendingICE = append(c.pendingICE, cd)
		return
	}
	_ = c.pc.AddICECandidate(cd)
}

func (c *client) setRemoteDescription(desc webrtc.SessionDescription) {
	if err := c.pc.SetRemoteDescription(desc); err != nil {
		log.Printf("set remote: %v", err)
	}
	c.iceMu.Lock()
	c.remoteDesc.Store(true)
	for _, cd := range c.pendingICE {
		_ = c.pc.AddICECandidate(cd)
	}
	c.pendingICE = nil
	c.iceMu.Unlock()
}

func (c *client) send(m msg) {
	c.seq++
	if m.RequestID == "" {
		m.RequestID = fmt.Sprintf("r%d", c.seq)
	}
	data, _ := json.Marshal(m)
	c.sendMu.Lock()
	defer c.sendMu.Unlock()
	if err := c.conn.Write(context.Background(), websocket.MessageText, data); err != nil {
		log.Printf("send: %v", err)
	}
}

func (c *client) waitType(want string) msg {
	t := time.NewTimer(15 * time.Second)
	defer t.Stop()
	for {
		select {
		case m, ok := <-c.recvCh:
			if !ok {
				log.Fatalf("conn closed waiting %s", want)
			}
			if m.Type == want {
				return m
			}
		case <-t.C:
			log.Fatalf("timeout waiting %s", want)
		}
	}
}

func (c *client) readLoop() {
	for {
		_, data, err := c.conn.Read(context.Background())
		if err != nil {
			close(c.recvCh)
			return
		}
		var m msg
		if json.Unmarshal(data, &m) != nil {
			continue
		}
		switch m.Type {
		case "sdp":
			if m.Jsep == "answer" && c.pc != nil {
				c.setRemoteDescription(webrtc.SessionDescription{Type: webrtc.SDPTypeAnswer, SDP: m.SDP})
			}
		case "ice":
			if m.Completed || m.Candidate == nil || c.pc == nil {
				continue
			}
			mid := m.Candidate.SDPMid
			mli := uint16(m.Candidate.SDPMLineIndex)
			log.Printf("remote cand: %s", m.Candidate.Candidate)
			c.addRemoteCandidate(webrtc.ICECandidateInit{
				Candidate: m.Candidate.Candidate, SDPMid: &mid, SDPMLineIndex: &mli,
			})
		case "notify":
			log.Printf("notify: %s", data)
		}
		c.recvCh <- m
	}
}

func dial(url string) *client {
	conn, _, err := websocket.Dial(context.Background(), url, nil)
	if err != nil {
		log.Fatalf("dial %s: %v", url, err)
	}
	c := &client{conn: conn, recvCh: make(chan msg, 128)}
	go c.readLoop()
	return c
}

func (c *client) hello(devID string) {
	c.send(msg{Type: "hello", DeviceID: devID, RequestID: "hello"})
	m := c.waitType("hello_result")
	if !m.OK {
		log.Fatalf("hello failed: %+v", m.Error)
	}
}

func (c *client) join(target map[string]string) msg {
	c.send(msg{Type: "join", RequestID: fmt.Sprintf("j%d", c.seq+1), Target: target})
	return c.waitType("join_result")
}

func (c *client) joinRoom(devID string, target map[string]string) msg {
	j := c.join(target)
	if !j.OK {
		log.Fatalf("[%s] join failed: %+v", devID, j.Error)
	}
	if j.NeedMedia {
		if err := c.startMedia(); err != nil {
			log.Fatalf("[%s] startMedia: %v", devID, err)
		}
	}
	deadline := time.Now().Add(15 * time.Second)
	for !c.connected.Load() && time.Now().Before(deadline) {
		time.Sleep(50 * time.Millisecond)
	}
	if !c.connected.Load() {
		log.Fatalf("[%s] webrtc not connected", devID)
	}
	c.send(msg{Type: "confirm", RequestID: fmt.Sprintf("cf%d", c.seq+1)})
	m := c.waitType("confirm_result")
	if !m.OK {
		log.Fatalf("[%s] confirm failed: %+v", devID, m.Error)
	}
	return m
}

var turnAddr string

func (c *client) startMedia() error {
	var err error
	cfg := webrtc.Configuration{}
	if turnAddr != "" {
		cfg.ICEServers = []webrtc.ICEServer{{
			URLs:       []string{"turn:" + turnAddr + "?transport=udp"},
			Username:   "intercom",
			Credential: "night2026",
		}}
	}
	c.pc, err = webrtc.NewPeerConnection(cfg)
	if err != nil {
		return err
	}
	c.pc.OnTrack(func(tr *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		go func() {
			for {
				pkt, _, err := tr.ReadRTP()
				if err != nil {
					return
				}
				c.rxPkts.Add(1)
				c.rxBytes.Add(int64(len(pkt.Payload)))
				if c.rec != nil {
					_ = c.rec.WriteRTP(pkt)
				}
			}
		}()
	})
	c.pc.OnConnectionStateChange(func(s webrtc.PeerConnectionState) {
		log.Printf("pc state: %s", s)
		if s == webrtc.PeerConnectionStateConnected {
			c.connected.Store(true)
		}
	})
	c.pc.OnICECandidate(func(cd *webrtc.ICECandidate) {
		if cd == nil {
			c.send(msg{Type: "ice", Completed: true})
			return
		}
		j := cd.ToJSON()
		log.Printf("local cand: %s", j.Candidate)
		mid, mli := "", uint16(0)
		if j.SDPMid != nil {
			mid = *j.SDPMid
		}
		if j.SDPMLineIndex != nil {
			mli = *j.SDPMLineIndex
		}
		c.send(msg{Type: "ice", Candidate: &iceCand{
			Candidate: j.Candidate, SDPMid: mid, SDPMLineIndex: int(mli)}})
	})
	c.track, err = webrtc.NewTrackLocalStaticSample(
		webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus}, "audio", "probe")
	if err != nil {
		return err
	}
	if _, err = c.pc.AddTrack(c.track); err != nil {
		return err
	}
	offer, err := c.pc.CreateOffer(nil)
	if err != nil {
		return err
	}
	if err = c.pc.SetLocalDescription(offer); err != nil {
		return err
	}
	c.send(msg{Type: "sdp", RequestID: fmt.Sprintf("sdp%d", c.seq+1), SDP: offer.SDP})
	return nil
}

func (c *client) streamOgg(path string) {
	go func() {
		for {
			f, err := os.Open(path)
			if err != nil {
				log.Printf("open %s: %v", path, err)
				return
			}
			ogg, _, err := oggreader.NewWith(f)
			if err != nil {
				log.Printf("ogg: %v", err)
				return
			}
			var last uint64
			for {
				pkt, hdr, err := ogg.ParseNextPage()
				if err == io.EOF {
					break
				}
				if err != nil {
					log.Printf("ogg parse: %v", err)
					return
				}
				var dur time.Duration
				if hdr.GranulePosition > last {
					dur = time.Duration((hdr.GranulePosition-last)*1000/48) * time.Microsecond
				} else {
					dur = 20 * time.Millisecond
				}
				last = hdr.GranulePosition
				if c.track != nil {
					if err := c.track.WriteSample(media.Sample{Data: pkt, Duration: dur}); err != nil {
						log.Printf("write sample: %v", err)
						return
					}
					c.txPkts.Add(1)
				}
				time.Sleep(dur)
			}
			f.Close()
		}
	}()
}

func main() {
	var (
		url     = flag.String("url", "ws://127.0.0.1:8090/ws", "control ws url")
		id      = flag.String("id", "probe-1", "device id")
		entry   = flag.String("entry", "", "nfc entry to join")
		invite  = flag.String("invite", "", "invite code to join")
		room    = flag.String("room", "", "room name to join")
		switchTo = flag.String("switch", "", "after listen, switch to room name")
		ogg     = flag.String("ogg", "", "ogg opus file to stream")
		record  = flag.String("record", "", "record downlink to ogg")
		listen  = flag.Duration("listen", 8*time.Second, "listen duration")
		info    = flag.Bool("info", false, "query room info after join")
		turn    = flag.String("turn", "", "turn server host:port")
	)
	flag.Parse()
	turnAddr = *turn

	if *record != "" {
		w, err := oggwriter.New(*record, 48000, 2)
		if err != nil {
			log.Fatalf("record: %v", err)
		}
		// keep process exit simple; close at end
		defer w.Close()
		defer func() {
			// assign after construction to avoid nil race
		}()
		go func() {}()
		// store into first client after creation below via global
		recGlobal = w
	}

	c := dial(*url)
	c.rec = recGlobal
	c.hello(*id)
	target := map[string]string{"kind": "nfc", "entry_id": *entry}
	if *invite != "" {
		target = map[string]string{"kind": "invite", "code": *invite}
	} else if *room != "" {
		target = map[string]string{"kind": "room_name", "name": *room}
	}
	m := c.joinRoom(*id, target)
	log.Printf("JOINED room=%s code=%s online=%d", m.Room.Name, m.Room.InviteCode, m.Room.OnlineCount)
	if *ogg != "" {
		c.streamOgg(*ogg)
	}
	if *info {
		c.send(msg{Type: "room_info", RequestID: "info"})
		ri := c.waitType("room_info_result")
		log.Printf("ROOMINFO name=%s online=%d code=%s digits=%s", ri.Name, ri.OnlineCount, ri.InviteCode, ri.InviteDigits)
	}
	time.Sleep(*listen)
	if *switchTo != "" {
		tgt := map[string]string{"kind": "room_name", "name": *switchTo}
		if strings.HasPrefix(*switchTo, "invite:") {
			tgt = map[string]string{"kind": "invite", "code": strings.TrimPrefix(*switchTo, "invite:")}
		}
		t0 := time.Now()
		j := c.join(tgt)
		log.Printf("SWITCH ok=%v room=%v err=%v took=%v", j.OK, j.Room.Name, j.Error, time.Since(t0).Round(time.Millisecond))
		if j.OK && !j.NeedMedia {
			c.send(msg{Type: "confirm", RequestID: "cfsw"})
			m := c.waitType("confirm_result")
			log.Printf("CONFIRMED %s online=%d", m.Room.Name, m.Room.OnlineCount)
		}
		time.Sleep(3 * time.Second)
	}
	log.Printf("RESULT rx_packets=%d rx_bytes=%d tx_packets=%d connected=%v", c.rxPkts.Load(), c.rxBytes.Load(), c.txPkts.Load(), c.connected.Load())
}

var recGlobal *oggwriter.OggWriter
