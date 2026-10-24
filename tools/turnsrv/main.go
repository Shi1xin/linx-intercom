// turnsrv: minimal TURN/relay server for the intercom test path.
// Runs on the Mac so LAN devices (breadboard, browser) can relay media
// to Janus on the tailnet. Long-term credential auth, single realm.
package main

import (
	"flag"
	"log"
	"net"
	"os"

	"github.com/pion/turn/v4"
)

func env(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func main() {
	var (
		addr  = flag.String("addr", "0.0.0.0:3478", "udp listen")
		publicIP = flag.String("public-ip", "127.0.0.1", "relay advertised ip")
		user  = flag.String("user", env("TURN_USER", "intercom"), "username")
		pass  = flag.String("pass", env("TURN_PASS", ""), "password (or TURN_PASS env)")
		realm = flag.String("realm", "intercom", "realm")
	)
	flag.Parse()
	if *pass == "" {
		log.Fatal("-pass or TURN_PASS required")
	}

	udp, err := net.ListenUDP("udp4", mustResolve(*addr))
	if err != nil {
		log.Fatalf("listen %s: %v", *addr, err)
	}
	srv, err := turn.NewServer(turn.ServerConfig{
		Realm: *realm,
		AuthHandler: func(username, realm string, srcAddr net.Addr) ([]byte, bool) {
			if username == *user {
				return turn.GenerateAuthKey(username, realm, *pass), true
			}
			return nil, false
		},
		PacketConnConfigs: []turn.PacketConnConfig{{
			PacketConn: udp,
			RelayAddressGenerator: &turn.RelayAddressGeneratorStatic{
				RelayAddress: net.ParseIP(*publicIP),
				Address:      "0.0.0.0",
			},
		}},
	})
	if err != nil {
		log.Fatalf("turn server: %v", err)
	}
	defer srv.Close()
	log.Printf("turn listening udp %s relay-ip %s user %s", *addr, *publicIP, *user)
	select {}
}

func mustResolve(addr string) *net.UDPAddr {
	a, err := net.ResolveUDPAddr("udp4", addr)
	if err != nil {
		log.Fatalf("resolve %s: %v", addr, err)
	}
	return a
}
