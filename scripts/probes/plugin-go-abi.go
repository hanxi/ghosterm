// P0 research ONLY. Linked into a C test executable, never into Ghosterm.
package main

// #include "plugin-go-abi.h"
import "C"

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httptest"
	"runtime"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"

	"github.com/jt-wang/tailpocket/connecthost"
	"github.com/jt-wang/tailpocket/connectinvite"
	"github.com/jt-wang/tailpocket/connectphone"
	"tailscale.com/derp/derpserver"
	"tailscale.com/net/stun"
	"tailscale.com/tailcfg"
	"tailscale.com/types/key"
)

func main() {}

type entry struct {
	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup
	busy   bool // guarded by registry.Mutex
}

var registry = struct {
	sync.Mutex
	next    uint64
	entries map[uint64]*entry
}{entries: make(map[uint64]*entry)}

// Handles are checked before use and never reused. No Go address is exposed.
// The registry lock protects both acquiring operations and starting Close.
func acquire(id uint64) (*entry, C.int) {
	registry.Lock()
	defer registry.Unlock()
	e := registry.entries[id]
	if e == nil {
		return nil, C.PGABI_INVALID
	}
	if e.busy {
		return nil, C.PGABI_BUSY
	}
	e.busy = true
	e.wg.Add(1)
	return e, C.PGABI_OK
}
func release(e *entry) {
	registry.Lock()
	e.busy = false
	e.wg.Done()
	registry.Unlock()
}

func buffers(input unsafe.Pointer, n C.uint32_t, output unsafe.Pointer, cap C.uint32_t, written *C.uint32_t) C.int {
	if written == nil {
		return C.PGABI_INVALID
	}
	*written = 0
	if n == 0 || n > 65536 {
		return C.PGABI_LIMIT
	}
	if input == nil || output == nil {
		return C.PGABI_INVALID
	}
	if cap < n {
		*written = n
		return C.PGABI_BUFFER
	}
	return C.PGABI_OK
}

//export pgabi_new
func pgabi_new() C.uint64_t {
	registry.Lock()
	defer registry.Unlock()
	registry.next++
	ctx, cancel := context.WithCancel(context.Background())
	registry.entries[registry.next] = &entry{ctx: ctx, cancel: cancel}
	return C.uint64_t(registry.next)
}

//export pgabi_copy
func pgabi_copy(id C.uint64_t, input unsafe.Pointer, n C.uint32_t, output unsafe.Pointer, cap C.uint32_t, written *C.uint32_t) C.int {
	if status := buffers(input, n, output, cap, written); status != C.PGABI_OK {
		return status
	}
	e, status := acquire(uint64(id))
	if status != C.PGABI_OK {
		return status
	}
	defer release(e)
	if e.ctx.Err() != nil {
		return C.PGABI_CANCELLED
	}
	data := C.GoBytes(input, C.int(n)) // synchronous copy, never retain C input
	copy(unsafe.Slice((*byte)(output), int(n)), data)
	*written = n
	return C.PGABI_OK
}

//export pgabi_run
func pgabi_run(id C.uint64_t, mode C.int, input unsafe.Pointer, n C.uint32_t, output unsafe.Pointer, cap C.uint32_t, written *C.uint32_t) C.int {
	if status := buffers(input, n, output, cap, written); status != C.PGABI_OK {
		return status
	}
	if mode != 0 && mode != 1 {
		return C.PGABI_INVALID
	}
	e, status := acquire(uint64(id))
	if status != C.PGABI_OK {
		return status
	}
	defer release(e)
	data := C.GoBytes(input, C.int(n))
	var result []byte
	var err error
	if mode == 1 {
		<-e.ctx.Done()
		err = e.ctx.Err()
	} else {
		ctx, cancel := context.WithTimeout(e.ctx, 25*time.Second)
		defer cancel()
		result, err = roundtrip(ctx, data)
	}
	if err != nil {
		if errors.Is(err, context.Canceled) {
			return C.PGABI_CANCELLED
		}
		if errors.Is(err, context.DeadlineExceeded) {
			return C.PGABI_TIMEOUT
		}
		return C.PGABI_NETWORK // do not return raw error text containing addresses
	}
	// Cancel is checked before the only output write. This does not prove
	// arbitrary concurrent cancellation linearization; that remains a gate.
	if e.ctx.Err() != nil {
		return C.PGABI_CANCELLED
	}
	copy(unsafe.Slice((*byte)(output), int(n)), result)
	*written = C.uint32_t(len(result))
	return C.PGABI_OK
}

//export pgabi_cancel
func pgabi_cancel(id C.uint64_t) C.int {
	registry.Lock()
	defer registry.Unlock()
	e := registry.entries[uint64(id)]
	if e == nil {
		return C.PGABI_INVALID
	}
	e.cancel()
	return C.PGABI_OK
}

//export pgabi_close
func pgabi_close(id C.uint64_t) C.int {
	registry.Lock()
	e := registry.entries[uint64(id)]
	if e != nil {
		delete(registry.entries, uint64(id))
	}
	registry.Unlock()
	if e == nil {
		return C.PGABI_INVALID
	}
	e.cancel()
	e.wg.Wait() // no C caller buffer is used after this returns
	return C.PGABI_OK
}

//export pgabi_snapshot
func pgabi_snapshot(out *C.pgabi_stats, collect C.int) C.int {
	if out == nil {
		return C.PGABI_INVALID
	}
	if collect != 0 {
		runtime.GC()
	}
	var memory runtime.MemStats
	runtime.ReadMemStats(&memory)
	registry.Lock()
	out.handles = C.uint64_t(len(registry.entries))
	out.active = 0
	for _, e := range registry.entries {
		if e.busy {
			out.active++
		}
	}
	registry.Unlock()
	out.goroutines = C.uint64_t(runtime.NumGoroutine())
	out.heap_alloc = C.uint64_t(memory.HeapAlloc)
	out.heap_sys = C.uint64_t(memory.HeapSys)
	out.cgo_calls = C.uint64_t(runtime.NumCgoCall())
	return C.PGABI_OK
}

// Real networking fixture, deliberately local. No accounts or user credentials,
// no key files, no TailPocket helper process, no changes to upstream sources.
func roundtrip(ctx context.Context, data []byte) ([]byte, error) {
	d := derpserver.New(key.NewNode(), func(string, ...any) {})
	defer d.Close()
	relay := httptest.NewUnstartedServer(derpserver.Handler(d))
	relay.Config.ErrorLog = log.New(io.Discard, "", 0)
	relay.Config.TLSNextProto = make(map[string]func(*http.Server, *tls.Conn, http.Handler))
	relay.StartTLS()
	defer relay.Close()
	stunSocket, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		return nil, err
	}
	defer stunSocket.Close()
	go func() {
		var buffer [2048]byte
		for {
			n, peer, err := stunSocket.ReadFromUDPAddrPort(buffer[:])
			if err != nil {
				return
			}
			tx, err := stun.ParseBindingRequest(buffer[:n])
			if err == nil {
				_, _ = stunSocket.WriteToUDPAddrPort(stun.Response(tx, peer), peer)
			}
		}
	}()
	dm := &tailcfg.DERPMap{Regions: map[tailcfg.DERPRegionID]*tailcfg.DERPRegion{1: {
		RegionID: 1, RegionCode: "abi-probe", Nodes: []*tailcfg.DERPNode{{
			Name: "abi-probe", RegionID: 1, HostName: "127.0.0.1", IPv4: "127.0.0.1", IPv6: "none",
			STUNPort: stunSocket.LocalAddr().(*net.UDPAddr).Port, STUNTestIP: "127.0.0.1",
			DERPPort: relay.Listener.Addr().(*net.TCPAddr).Port, InsecureForTests: true,
		}},
	}}}
	maps := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewEncoder(w).Encode(dm)
	}))
	defer maps.Close()
	var saved atomic.Int64
	host, err := connecthost.New(connecthost.Config{Name: "C ABI fixture", DERPMapURL: maps.URL,
		Save: func(state connecthost.State) error { saved.Store(int64(len(state.Allowed))); return nil }})
	if err != nil {
		return nil, err
	}
	defer host.Close()
	if err := host.Start(ctx); err != nil {
		return nil, err
	}
	var workers sync.WaitGroup
	defer workers.Wait() // registered before Close below so listener is closed first
	defer host.Close()
	workers.Add(1)
	go func() {
		defer workers.Done()
		conn, err := host.Listener().Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		_ = conn.SetDeadline(time.Now().Add(10 * time.Second))
		_, _ = io.Copy(conn, conn)
	}()
	window, err := host.Pair(ctx, connecthost.PairOptions{TTL: 45 * time.Second})
	if err != nil {
		return nil, err
	}
	defer window.Close()
	text, err := window.QR(connectinvite.DefaultPrefix)
	if err != nil {
		return nil, err
	}
	invite, err := connectinvite.Decode(text, connectinvite.DefaultPrefix)
	if err != nil {
		return nil, err
	}
	link, err := connectphone.Pair(ctx, invite, connectphone.NewKey(), connectphone.Options{DERPMapURL: maps.URL})
	if err != nil {
		return nil, err
	}
	peer, err := window.Wait(ctx)
	if err != nil {
		return nil, err
	}
	public, err := connectphone.PublicKey(link.Key)
	if err != nil {
		return nil, err
	}
	if peer.Key != public || saved.Load() != 1 {
		return nil, errors.New("binding")
	}
	window.Close()
	session, err := connectphone.Connect(link, connectphone.Options{DERPMapURL: maps.URL})
	if err != nil {
		return nil, err
	}
	defer session.Close()
	conn, err := session.Dial(ctx)
	if err != nil {
		return nil, err
	}
	defer conn.Close()
	if err := conn.SetDeadline(time.Now().Add(10 * time.Second)); err != nil {
		return nil, err
	}
	if _, err := conn.Write(data); err != nil {
		return nil, err
	}
	result := make([]byte, len(data))
	if _, err := io.ReadFull(conn, result); err != nil {
		return nil, err
	}
	var counters map[string]any
	_ = json.Unmarshal([]byte(d.ExpVar(false).String()), &counters)
	if counters["packets_sent"].(float64) <= 0 {
		return nil, errors.New("not relayed")
	}
	return result, nil
}
