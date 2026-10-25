package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/shi1xin/linx-intercom/server/internal/server"
)

func env(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func main() {
	var (
		addr        = flag.String("addr", env("ADDR", ":8090"), "listen address")
		janusURL    = flag.String("janus", env("JANUS_URL", ""), "janus websocket url, e.g. ws://127.0.0.1:8188")
		dbPath      = flag.String("db", env("DB_PATH", "intercom.db"), "sqlite path")
		deviceToken = flag.String("device-token", env("DEVICE_TOKEN", ""), "required device token")
		adminToken  = flag.String("admin-token", env("ADMIN_TOKEN", ""), "admin endpoint token")
		mcpEnable   = flag.Bool("mcp", env("MCP", "1") == "1", "enable cloud MCP (SSE) endpoints /mcp + /mcp/message")
		mcpTarget   = flag.String("mcp-target", env("MCP_TARGET", ""), "default target device id for cloud MCP calls")
		mcpToken    = flag.String("mcp-token", env("MCP_TOKEN", ""), "shared secret for /mcp endpoints (empty = open, dev only)")
	)
	flag.Parse()

	store, err := server.OpenStore(*dbPath)
	if err != nil {
		log.Fatalf("open store: %v", err)
	}
	defer store.Close()

	var srv *server.ControlServer
	var media server.Media
	if *janusURL != "" {
		dial := func(ctx context.Context) (*server.JanusClient, error) {
			jc, err := server.DialJanus(ctx, *janusURL)
			if err == nil {
				log.Printf("janus connected: %s", *janusURL)
			}
			return jc, err
		}
		media = server.NewLiveMedia(dial, func() {
			if srv != nil {
				srv.ClearAllMedia()
			}
		})
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		if jc, err := dial(ctx); err != nil {
			log.Printf("janus initial dial failed (will retry on first use): %v", err)
		} else {
			jc.Close() // connectivity check only; LiveMedia dials lazily
		}
		cancel()
	} else {
		media = server.NewFakeMedia()
		log.Printf("JANUS_URL unset: using fake media (protocol tests only)")
	}

	srv = server.NewControlServer(server.Config{
		Addr:        *addr,
		JanusURL:    *janusURL,
		DeviceToken: *deviceToken,
		AdminToken:  *adminToken,
	}, store, media)
	if *mcpEnable {
		srv.AttachMCP(*mcpTarget, *mcpToken)
		if *mcpToken == "" {
			log.Printf("WARNING: mcp enabled without -mcp-token: /mcp endpoints accept unauthenticated calls")
		}
		log.Printf("mcp adapter on /mcp (default target %q)", *mcpTarget)
	}

	go func() {
		ch := make(chan os.Signal, 1)
		signal.Notify(ch, syscall.SIGINT, syscall.SIGTERM)
		<-ch
		log.Printf("shutting down")
		os.Exit(0)
	}()

	log.Fatal(srv.ListenAndServe())
}
