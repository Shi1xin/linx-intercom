package server

import (
	"context"
	"fmt"
	"sync"
)

// FakeMedia implements Media without a real Janus, for protocol and store tests.
// It records calls and returns canned answers; Join answer is a fixed string.
type FakeMedia struct {
	mu        sync.Mutex
	Calls     []string
	FailNext  error
	AnswerSDP string
	handleSeq int64
	handler   func(sessionID, handleID int64, msg AsyncMsg)
}

func NewFakeMedia() *FakeMedia {
	return &FakeMedia{AnswerSDP: "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=fake\r\nt=0 0\r\nm=audio 9 RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n"}
}

func (f *FakeMedia) record(s string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.Calls = append(f.Calls, s)
	if f.FailNext != nil {
		err := f.FailNext
		f.FailNext = nil
		return err
	}
	return nil
}

// SetFailNext makes the next media call return err (thread-safe).
func (f *FakeMedia) SetFailNext(err error) {
	f.mu.Lock()
	f.FailNext = err
	f.mu.Unlock()
}

func (f *FakeMedia) CallsSnapshot() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]string(nil), f.Calls...)
}

func (f *FakeMedia) EnsureRoom(ctx context.Context, janusRoom int64) error {
	return f.record(fmt.Sprintf("ensure %d", janusRoom))
}

func (f *FakeMedia) Join(ctx context.Context, p *Participant, janusRoom int64, display, offer string) (string, error) {
	f.mu.Lock()
	f.handleSeq++
	p.SessionID = f.handleSeq*100 + 1
	p.HandleID = f.handleSeq*100 + 2
	p.JanusRoom = janusRoom
	f.mu.Unlock()
	if err := f.record(fmt.Sprintf("join %d %s", janusRoom, display)); err != nil {
		return "", err
	}
	return f.AnswerSDP, nil
}

func (f *FakeMedia) ChangeRoom(ctx context.Context, p *Participant, janusRoom int64) error {
	if err := f.record(fmt.Sprintf("changeroom %d->%d", p.JanusRoom, janusRoom)); err != nil {
		return err
	}
	p.JanusRoom = janusRoom
	return nil
}

func (f *FakeMedia) SetMuted(ctx context.Context, p *Participant, muted bool) error {
	return f.record(fmt.Sprintf("mute %v", muted))
}

func (f *FakeMedia) Leave(ctx context.Context, p *Participant) error {
	return f.record("leave")
}

func (f *FakeMedia) Trickle(ctx context.Context, p *Participant, cand *IceCandidate, completed bool) error {
	return f.record("trickle")
}

func (f *FakeMedia) Detach(ctx context.Context, p *Participant) error {
	return f.record("detach")
}

func (f *FakeMedia) SetEventHandler(fn func(sessionID, handleID int64, msg AsyncMsg)) {
	f.handler = fn
}

func (f *FakeMedia) Close() error { return nil }
