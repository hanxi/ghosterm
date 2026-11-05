// P0 reference ONLY: upstream Go networking, never linked or packaged in Ghosterm.
// All private fixture keys/invitations remain in this process's memory.
package main

import (
	"bufio"
	"context"
	"crypto/tls"
	"encoding/json"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"github.com/jt-wang/tailpocket/connecthost"
	"github.com/jt-wang/tailpocket/connectinvite"
	"github.com/jt-wang/tailpocket/connectphone"
	"tailscale.com/derp/derpserver"
	"tailscale.com/net/stun"
	"tailscale.com/tailcfg"
	"tailscale.com/types/key"
)

func must(err error) {
	if err != nil {
		panic("reference operation failed (details suppressed to protect credentials)")
	}
}

var outputMu sync.Mutex

func emit(value any) {
	outputMu.Lock()
	defer outputMu.Unlock()
	must(json.NewEncoder(os.Stdout).Encode(value))
}

func main() {
	if len(os.Args) != 2 {
		panic("executor Unix socket path required")
	}
	log.SetOutput(io.Discard)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	derp := derpserver.New(key.NewNode(), func(string, ...any) {})
	defer derp.Close()
	var derpRequests atomic.Int64
	handler := derpserver.Handler(derp)
	relay := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		derpRequests.Add(1)
		handler.ServeHTTP(w, r)
	}))
	relay.Config.ErrorLog = log.New(io.Discard, "", 0)
	relay.Config.TLSNextProto = make(map[string]func(*http.Server, *tls.Conn, http.Handler))
	relay.StartTLS()
	defer relay.Close()
	// As in upstream's DERP integration fixture, local STUN gives netcheck a
	// real latency sample; no public STUN server or HTTPS latency probe needed.
	stunSocket, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	must(err)
	defer stunSocket.Close()
	go func() {
		var buffer [2048]byte
		for {
			n, source, err := stunSocket.ReadFromUDPAddrPort(buffer[:])
			if err != nil {
				return
			}
			transaction, err := stun.ParseBindingRequest(buffer[:n])
			if err == nil {
				_, _ = stunSocket.WriteToUDPAddrPort(stun.Response(transaction, source), source)
			}
		}
	}()
	dm := &tailcfg.DERPMap{Regions: map[tailcfg.DERPRegionID]*tailcfg.DERPRegion{
		1: {RegionID: 1, RegionCode: "isolated", Nodes: []*tailcfg.DERPNode{{
			Name: "isolated", RegionID: 1, HostName: "127.0.0.1", IPv4: "127.0.0.1", IPv6: "none",
			STUNPort: stunSocket.LocalAddr().(*net.UDPAddr).Port, STUNTestIP: "127.0.0.1",
			DERPPort: relay.Listener.Addr().(*net.TCPAddr).Port, InsecureForTests: true,
		}}},
	}}
	maps := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { must(json.NewEncoder(w).Encode(dm)) }))
	defer maps.Close()
	var state connecthost.State // synthetic secrets: no file serialization
	var stateMu sync.Mutex
	savedCount := func() int { stateMu.Lock(); defer stateMu.Unlock(); return len(state.Allowed) }
	host, err := connecthost.New(connecthost.Config{Name: "Ghosterm reference fixture", DERPMapURL: maps.URL,
		Save: func(next connecthost.State) error { stateMu.Lock(); state = next; stateMu.Unlock(); return nil },
	})
	must(err)
	must(host.Start(ctx))
	defer host.Close()
	var accepted atomic.Int64
	var live sync.Map
	defer live.Range(func(k, _ any) bool { _ = k.(net.Conn).Close(); return true })
	go func() {
		for {
			conn, err := host.Listener().Accept()
			if err != nil {
				return
			}
			accepted.Add(1)
			live.Store(conn, true)
			go func(conn net.Conn) {
				defer live.Delete(conn)
				defer conn.Close()
				endpoint, err := net.DialTimeout("unix", os.Args[1], time.Second)
				if err != nil {
					return
				}
				defer endpoint.Close()
				done := make(chan struct{}, 2)
				go func() { _, _ = io.Copy(endpoint, conn); done <- struct{}{} }()
				go func() { _, _ = io.Copy(conn, endpoint); done <- struct{}{} }()
				<-done
				_ = conn.Close()
				_ = endpoint.Close()
				<-done
			}(conn)
		}
	}()
	var link connectphone.Link
	var session *connectphone.Session
	var forward *connectphone.Forward
	defer func() {
		if forward != nil {
			forward.Close()
		}
		if session != nil {
			_ = session.Close()
		}
	}()
	emit(map[string]any{"event": "ready", "privateKeysOnDisk": false})
	scanner := bufio.NewScanner(os.Stdin)
	for scanner.Scan() {
		switch scanner.Text() {
		case "pair":
			emit(map[string]any{"event": "stage", "phase": "pair-window-start"})
			windowCtx, windowCancel := context.WithTimeout(ctx, 15*time.Second)
			window, err := host.Pair(windowCtx, connecthost.PairOptions{TTL: 60 * time.Second})
			windowCancel()
			must(err)
			emit(map[string]any{"event": "stage", "phase": "pair-window-ready"})
			encoded, err := window.QR(connectinvite.DefaultPrefix)
			must(err)
			invite, err := connectinvite.Decode(encoded, connectinvite.DefaultPrefix)
			must(err)
			cctx, ccancel := context.WithTimeout(ctx, 15*time.Second)
			link, err = connectphone.Pair(cctx, invite, connectphone.NewKey(), connectphone.Options{DERPMapURL: maps.URL})
			ccancel()
			must(err)
			emit(map[string]any{"event": "stage", "phase": "pair-accepted"})
			phone, err := window.Wait(ctx)
			must(err)
			public, err := connectphone.PublicKey(link.Key)
			must(err)
			if phone.Key != public || savedCount() != 1 || host.Allowed()[0] != public {
				panic("pair binding mismatch")
			}
			window.Close()
			session, err = connectphone.Connect(link, connectphone.Options{DERPMapURL: maps.URL})
			must(err)
			forward, err = session.Forward()
			must(err)
			emit(map[string]any{"event": "paired", "port": forward.Port(), "allowed": savedCount()})
		case "unpaired":
			before := accepted.Load()
			strangerLink := link
			strangerLink.Key = connectphone.NewKey()
			stranger, err := connectphone.Connect(strangerLink, connectphone.Options{DERPMapURL: maps.URL})
			must(err)
			cctx, ccancel := context.WithTimeout(ctx, 2*time.Second)
			conn, dialErr := stranger.Dial(cctx)
			ccancel()
			if conn != nil {
				_ = conn.Close()
			}
			_ = stranger.Close()
			if dialErr == nil || accepted.Load() != before {
				panic("unpaired key reached service")
			}
			emit(map[string]any{"event": "unpaired-rejected"})
		case "probe-after-exit":
			// Distinguish an exited application from a revoked network route.
			// The virtual TCP service still accepts, then closes because the
			// executor's Unix listener has gone. No plugin operation is sent.
			before := accepted.Load()
			cctx, ccancel := context.WithTimeout(ctx, 3*time.Second)
			conn, err := session.Dial(cctx)
			ccancel()
			must(err)
			must(conn.SetReadDeadline(time.Now().Add(3 * time.Second)))
			var byte [1]byte
			n, readErr := conn.Read(byte[:])
			_ = conn.Close()
			if n != 0 || readErr != io.EOF || accepted.Load() != before+1 {
				panic("executor exit/network control mismatch")
			}
			emit(map[string]any{"event": "route-live-executor-gone", "accepted": accepted.Load()})
		case "stats":
			emit(map[string]any{"event": "stats", "accepted": accepted.Load(), "derpRequests": derpRequests.Load(),
				"derpCounters": derp.ExpVar(false).String()})
		case "revoke":
			public, err := connectphone.PublicKey(link.Key)
			must(err)
			must(host.Revoke(ctx, public))
			if savedCount() != 0 || len(host.Allowed()) != 0 {
				panic("revoke failed")
			}
			// Fresh independent client with the old link must fail to reach host.Listener.
			before := accepted.Load()
			old, err := connectphone.Connect(link, connectphone.Options{DERPMapURL: maps.URL})
			must(err)
			cctx, ccancel := context.WithTimeout(ctx, 2*time.Second)
			conn, dialErr := old.Dial(cctx)
			ccancel()
			if conn != nil {
				_ = conn.Close()
			}
			_ = old.Close()
			if dialErr == nil || accepted.Load() != before {
				panic("revoked link reached service")
			}
			emit(map[string]any{"event": "revoked", "freshDialRejected": true, "accepted": accepted.Load()})
		case "stop":
			emit(map[string]any{"event": "stopped"})
			return
		default:
			panic("unknown reference command")
		}
	}
	must(scanner.Err())
}
