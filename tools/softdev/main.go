// softdev: software Linx-intercom device.
//
// Acts as the firmware stand-in for stage-1 verification: it holds the Linx
// platform WebSocket as an MCP tool server and the intercom control WebSocket
// as a regular device. Platform tools/call (or stdin ops) drive real room
// joins with real WebRTC media through Janus AudioBridge.
//
// Usage:
//
//	softdev -ota ota-bound.json \
//	  -device-id <device-mac> -client-id <platform-client-uuid> \
//	  -ctrl ws://<host>:8090/ws -prompt "请调用加入对讲房间工具，邀请码…"
//
// Stdin accepts {"tool":"self.intercom.join","arguments":{...}} lines and runs
// them through the same executor, printing STDIN_OP_RESULT lines.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"time"
)

func env(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func (d *softDevice) modeSnapshot() string {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.mode == "intercom" && d.room != nil {
		return "intercom:" + d.room.Name
	}
	return d.mode
}

type scriptStep struct {
	Text    string `json:"text"`
	DelayMs int    `json:"delay_ms"`
}

func loadScript(path string) []scriptStep {
	if path == "" {
		return nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		log.Fatalf("script: %v", err)
	}
	var steps []scriptStep
	if err := json.Unmarshal(data, &steps); err != nil || len(steps) == 0 {
		log.Fatalf("script parse: %v", err)
	}
	return steps
}

// runScript sends one detect text per round: the next prompt goes out after
// the previous reply's tts stop (+delay_ms), or a 35 s no-reply timeout.
func runScript(ctx context.Context, lx *linxConn, steps []scriptStep,
	ready <-chan struct{}, tts <-chan string) {
	for i, st := range steps {
		if i == 0 {
			select {
			case <-ready:
			case <-ctx.Done():
				return
			}
		} else {
			// wait for the previous round's tts stop; bail at timeout
			wait := time.NewTimer(35 * time.Second)
			seen := false
			for !seen {
				select {
				case s := <-tts:
					if s == "stop" {
						seen = true
					}
				case <-wait.C:
					seen = true
				case <-ctx.Done():
					wait.Stop()
					return
				}
			}
			wait.Stop()
		}
		if st.DelayMs > 0 {
			select {
			case <-time.After(time.Duration(st.DelayMs) * time.Millisecond):
			case <-ctx.Done():
				return
			}
		}
		lx.sendDetectText(st.Text)
		log.Printf("sent detect prompt %d/%d: %s", i+1, len(steps), st.Text)
	}
}

func stdinOps(d *softDevice) {
	sc := bufio.NewScanner(os.Stdin)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		var req struct {
			Tool      string          `json:"tool"`
			Arguments json.RawMessage `json:"arguments"`
		}
		if json.Unmarshal(sc.Bytes(), &req) != nil || req.Tool == "" {
			continue
		}
		var args map[string]any
		_ = json.Unmarshal(req.Arguments, &args)
		d.trace("stdin-op", map[string]any{"tool": req.Tool, "args": args})
		res, err := d.execOp(req.Tool, args)
		out, _ := json.Marshal(map[string]any{"result": res, "error": errString(err)})
		fmt.Printf("STDIN_OP_RESULT %s\n", out)
	}
}

func main() {
	var (
		otaFile  = flag.String("ota", "", "ota-bound.json path (websocket url+token)")
		linxURL  = flag.String("linx-url", "", "platform ws url (overrides ota file)")
		linxTok  = flag.String("linx-token", "", "platform device token (overrides ota file)")
		devID    = flag.String("device-id", "", "device MAC id")
		clientID = flag.String("client-id", "", "platform client uuid")
		ctrlURL  = flag.String("ctrl", "ws://127.0.0.1:8090/ws", "intercom control ws url")
		ctrlTok  = flag.String("ctrl-token", "dev", "intercom device token")
		turn     = flag.String("turn", "", "turn host:port for media fallback")
		turnUser = flag.String("turn-user", env("TURN_USER", "intercom"), "turn username")
		turnPass = flag.String("turn-pass", env("TURN_PASS", ""), "turn password")
		ogg      = flag.String("ogg", "", "opus ogg to stream while in room")
		prompt   = flag.String("prompt", "", "listen.detect text to send after tools/list")
		script   = flag.String("script", "", "json file [{\"text\":...,\"delay_ms\":N}] — one detect per round, paced on tts stop")
		duration = flag.Duration("duration", 60*time.Second, "max session duration")
		resume   = flag.Bool("resume", true, "auto rejoin resume_room after hello")
		trace    = flag.String("trace", "", "write full trace json to this path")
		summary  = flag.String("summary", "", "write summary json to this path")
		noLinx   = flag.Bool("no-linx", false, "skip platform connection (stdin ops only)")
		noDevMCP = flag.Bool("no-devmcp", false, "connect to platform but do not advertise device mcp tools")
	)
	flag.Parse()
	log.SetFlags(log.Ltime | log.Lmicroseconds)

	url, tok := *linxURL, *linxTok
	if *otaFile != "" {
		data, err := os.ReadFile(*otaFile)
		if err != nil {
			log.Fatalf("ota file: %v", err)
		}
		var ota struct {
			Websocket struct {
				URL   string `json:"url"`
				Token string `json:"token"`
			} `json:"websocket"`
		}
		if err := json.Unmarshal(data, &ota); err != nil {
			log.Fatalf("ota parse: %v", err)
		}
		if url == "" {
			url = ota.Websocket.URL
		}
		if tok == "" {
			tok = ota.Websocket.Token
		}
	}

	d := &softDevice{mode: "linx", deviceID: *devID, token: *ctrlTok,
		turnAddr: *turn, turnUser: *turnUser, turnPass: *turnPass,
		oggPath: *ogg, started: time.Now()}

	ctrl, err := dialCtrl(*ctrlURL)
	if err != nil {
		log.Fatalf("ctrl dial: %v", err)
	}
	defer ctrl.close()
	ctrl.dev = d
	d.ctrl = ctrl

	h, err := ctrl.hello(*devID, *ctrlTok)
	if err != nil {
		log.Fatalf("ctrl hello: %v", err)
	}
	if h.Muted != nil {
		d.muted = *h.Muted
	}
	log.Printf("ctrl hello ok muted=%v resume=%+v", d.muted, h.ResumeRoom)
	if *resume && h.ResumeRoom != nil {
		log.Printf("auto-resume room %s", h.ResumeRoom.Name)
		res, err := d.execOp("self.intercom.switch", map[string]any{"room_name": h.ResumeRoom.Name})
		log.Printf("resume result=%v err=%v", res, err)
	}

	var calls []string
	sum := map[string]any{"device_id": *devID}
	ctx, cancel := context.WithTimeout(context.Background(), *duration)
	defer cancel()

	if *noLinx {
		stdinOps(d)
	} else {
		if url == "" || tok == "" || *devID == "" || *clientID == "" {
			log.Fatalf("linx url/token/device-id/client-id required (or -no-linx)")
		}
		lx, err := dialLinx(url, tok, *devID, *clientID, !*noDevMCP)
		if err != nil {
			log.Fatalf("linx dial: %v", err)
		}
		defer lx.close()
		lx.dev = d
		lx.hello()
		go stdinOps(d)

		callDone := make(chan string, 8)
		runDone := make(chan error, 1)
		promptOnce := make(chan struct{}, 1)
		ttsStop := make(chan string, 16)
		go func() {
			runDone <- lx.run(ctx,
				func() {
					select {
					case promptOnce <- struct{}{}:
					default:
					}
				},
				func(name string) { callDone <- name },
				func(state string) {
					select {
					case ttsStop <- state:
					default:
					}
				})
		}()
		steps := loadScript(*script)
		scriptDone := make(chan struct{})
		go func() {
			defer close(scriptDone)
			if *noDevMCP {
				// platform never runs tools/list without the mcp feature;
				// give the session a moment, then send the detect text
				select {
				case <-time.After(1500 * time.Millisecond):
					promptOnce <- struct{}{}
				case <-ctx.Done():
					return
				}
			}
			if len(steps) > 0 {
				runScript(ctx, lx, steps, promptOnce, ttsStop)
				return
			}
			select {
			case <-promptOnce:
				if *prompt != "" {
					lx.sendDetectText(*prompt)
					log.Printf("sent detect prompt: %s", *prompt)
				}
			case <-ctx.Done():
			}
		}()

		// exit 6s after the last completed tool call (allow agent TTS tail),
		// otherwise at -duration; a script in flight keeps the session open
		scriptOpen := len(steps) > 0
		idle := time.NewTimer(time.Hour)
		idle.Stop()
	wait:
		for {
			select {
			case name := <-callDone:
				calls = append(calls, name)
				idle.Reset(6 * time.Second)
			case <-idle.C:
				if !scriptOpen {
					break wait
				}
				select {
				case <-scriptDone:
					scriptOpen = false
					idle.Reset(10 * time.Second)
				default:
					idle.Reset(time.Second)
				}
			case <-scriptDone:
				scriptOpen = false
				idle.Reset(10 * time.Second)
			case err := <-runDone:
				log.Printf("linx conn ended: %v", err)
				break wait
			case <-ctx.Done():
				break wait
			}
		}
		sum["audio_frames"] = d.audioFrames.Load()
	}
	cancel()

	sum["tool_calls"] = calls
	sum["mode"] = d.modeSnapshot()
	if m := d.currentMedia(); m != nil {
		sum["rx_packets"] = m.rxPkts.Load()
		sum["tx_packets"] = m.txPkts.Load()
	}
	sum["elapsed_seconds"] = time.Since(d.started).Seconds()

	if *trace != "" {
		d.traceMu.Lock()
		data, _ := json.MarshalIndent(d.traceLog, "", "  ")
		d.traceMu.Unlock()
		if err := os.WriteFile(*trace, data, 0o644); err != nil {
			log.Printf("trace write: %v", err)
		}
	}
	if *summary != "" {
		data, _ := json.MarshalIndent(sum, "", "  ")
		if err := os.WriteFile(*summary, data, 0o644); err != nil {
			log.Printf("summary write: %v", err)
		}
	}
	out, _ := json.Marshal(sum)
	fmt.Printf("SUMMARY %s\n", out)
}
