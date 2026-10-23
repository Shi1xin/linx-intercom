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
	)
	flag.Parse()

	store, err := server.OpenStore(*dbPath)
	if err != nil {
		log.Fatalf("open store: %v", err)
	}
	defer store.Close()

	var media server.Media
	if *janusURL != "" {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		jc, err := server.DialJanus(ctx, *janusURL)
		cancel()
		if err != nil {
			log.Fatalf("dial janus %s: %v", *janusURL, err)
		}
		media = server.NewJanusMedia(jc, "")
		log.Printf("janus connected: %s", *janusURL)
	} else {
		media = server.NewFakeMedia()
		log.Printf("JANUS_URL unset: using fake media (protocol tests only)")
	}

	srv := server.NewControlServer(server.Config{
		Addr:        *addr,
		JanusURL:    *janusURL,
		DeviceToken: *deviceToken,
		AdminToken:  *adminToken,
	}, store, media)

	go func() {
		ch := make(chan os.Signal, 1)
		signal.Notify(ch, syscall.SIGINT, syscall.SIGTERM)
		<-ch
		log.Printf("shutting down")
		os.Exit(0)
	}()

	log.Fatal(srv.ListenAndServe())
}
