package main

// WebRTC media session for the software device: one PeerConnection per room
// attach, Opus uplink from an ogg file or silence, downlink packet counting.

import (
	"io"
	"log"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/webrtc/v4"
	"github.com/pion/webrtc/v4/pkg/media"
	"github.com/pion/webrtc/v4/pkg/media/oggreader"
)

type mediaSession struct {
	dev *softDevice
	pc  *webrtc.PeerConnection
	trk *webrtc.TrackLocalStaticSample

	connected  atomic.Bool
	remoteDesc atomic.Bool
	pendingICE []webrtc.ICECandidateInit
	iceMu      sync.Mutex
	done       chan struct{}
	closeOnce  sync.Once

	rxPkts  atomic.Int64
	rxBytes atomic.Int64
	txPkts  atomic.Int64
}

func newMedia(dev *softDevice, turnAddr, turnUser, turnPass string) (*mediaSession, error) {
	m := &mediaSession{dev: dev, done: make(chan struct{})}
	cfg := webrtc.Configuration{}
	if turnAddr != "" {
		cfg.ICEServers = []webrtc.ICEServer{{
			URLs:       []string{"turn:" + turnAddr + "?transport=udp"},
			Username:   turnUser,
			Credential: turnPass,
		}}
	}
	pc, err := webrtc.NewPeerConnection(cfg)
	if err != nil {
		return nil, err
	}
	m.pc = pc
	pc.OnTrack(func(tr *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		go func() {
			for {
				pkt, _, err := tr.ReadRTP()
				if err != nil {
					return
				}
				m.rxPkts.Add(1)
				m.rxBytes.Add(int64(len(pkt.Payload)))
			}
		}()
	})
	pc.OnConnectionStateChange(func(s webrtc.PeerConnectionState) {
		log.Printf("pc state: %s", s)
		switch s {
		case webrtc.PeerConnectionStateConnected:
			m.connected.Store(true)
		case webrtc.PeerConnectionStateFailed, webrtc.PeerConnectionStateClosed:
			m.closeOnce.Do(func() { close(m.done) })
		}
	})
	pc.OnICECandidate(func(cd *webrtc.ICECandidate) {
		if cd == nil {
			m.dev.ctrl.send(ctrlMsg{Type: "ice", Completed: true})
			return
		}
		j := cd.ToJSON()
		mid, mli := "", uint16(0)
		if j.SDPMid != nil {
			mid = *j.SDPMid
		}
		if j.SDPMLineIndex != nil {
			mli = *j.SDPMLineIndex
		}
		m.dev.ctrl.send(ctrlMsg{Type: "ice", Candidate: &iceCand{
			Candidate: j.Candidate, SDPMid: mid, SDPMLineIndex: int(mli)}})
	})
	m.trk, err = webrtc.NewTrackLocalStaticSample(
		webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus}, "audio", "softdev")
	if err != nil {
		return nil, err
	}
	if _, err = pc.AddTrack(m.trk); err != nil {
		return nil, err
	}
	return m, nil
}

// offer creates and sends the SDP offer through the control channel.
func (m *mediaSession) offer() error {
	offer, err := m.pc.CreateOffer(nil)
	if err != nil {
		return err
	}
	if err = m.pc.SetLocalDescription(offer); err != nil {
		return err
	}
	m.dev.ctrl.send(ctrlMsg{Type: "sdp", SDP: offer.SDP})
	return nil
}

func (m *mediaSession) setAnswer(sdp string) {
	if err := m.pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeAnswer, SDP: sdp}); err != nil {
		log.Printf("set remote: %v", err)
		return
	}
	m.iceMu.Lock()
	m.remoteDesc.Store(true)
	for _, cd := range m.pendingICE {
		_ = m.pc.AddICECandidate(cd)
	}
	m.pendingICE = nil
	m.iceMu.Unlock()
}

func (m *mediaSession) addCandidate(c iceCand) {
	m.iceMu.Lock()
	defer m.iceMu.Unlock()
	mid := c.SDPMid
	mli := uint16(c.SDPMLineIndex)
	init := webrtc.ICECandidateInit{Candidate: c.Candidate, SDPMid: &mid, SDPMLineIndex: &mli}
	if !m.remoteDesc.Load() {
		m.pendingICE = append(m.pendingICE, init)
		return
	}
	_ = m.pc.AddICECandidate(init)
}

func (m *mediaSession) waitConnected(timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for !m.connected.Load() && time.Now().Before(deadline) {
		select {
		case <-m.done:
			return false
		default:
			time.Sleep(50 * time.Millisecond)
		}
	}
	return m.connected.Load()
}

// streamOgg loops an opus ogg file onto the uplink track.
func (m *mediaSession) streamOgg(path string) {
	go func() {
		for {
			select {
			case <-m.done:
				return
			default:
			}
			f, err := os.Open(path)
			if err != nil {
				log.Printf("open %s: %v", path, err)
				return
			}
			ogg, _, err := oggreader.NewWith(f)
			if err != nil {
				log.Printf("ogg: %v", err)
				f.Close()
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
					break
				}
				var dur time.Duration
				if hdr.GranulePosition > last {
					dur = time.Duration((hdr.GranulePosition-last)*1000/48) * time.Microsecond
				} else {
					dur = 20 * time.Millisecond
				}
				last = hdr.GranulePosition
				if err := m.trk.WriteSample(media.Sample{Data: pkt, Duration: dur}); err != nil {
					f.Close()
					return
				}
				m.txPkts.Add(1)
				time.Sleep(dur)
			}
			f.Close()
		}
	}()
}

func (m *mediaSession) teardown() {
	m.closeOnce.Do(func() { close(m.done) })
	if m.pc != nil {
		_ = m.pc.Close()
	}
}
