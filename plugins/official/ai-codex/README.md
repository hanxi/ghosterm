# Official Codex adapter

The P3 protocol adapter is in `protocol/client.ts`; it handles initialization,
thread / turn request correlation, streamed notifications, approval request IDs,
and unknown outcomes on pipe closure. `protocol/conversation.ts` layers one-thread
state, ordered event sequence, active-turn interrupt, approval expiry, and
unknown-on-close behavior over the client. `protocol/line-decoder.ts` frames bounded
UTF-8 JSON lines without Node or DOM APIs. `protocol/process.ts` launches the
Host mapping named `codex` through `PluginContext.process` after checking the
Executor role, current `process.spawn` / `process.pipes` grants, and opaque
authorized workspace handle. It returns the adapter with explicit bounded
`step()` and `stop()` lifecycle. macOS and Android product Hosts now register the
Codex RPC allowlist, receive bounded `codex.event` messages, and provide native
thread / turn controls, approval decisions, activity timelines, and raw-event
inspection. Launch still requires the user to grant a workspace and configure the
`codex` executable mapping with its expected version and SHA-256.

`executor/index.ts` is the current Native-independent Executor entry. It lazily
spawns only after `codex.thread.start`, exposes typed thread / turn / interrupt /
approval RPC handlers, and returns bounded ordered app-server events through
`codex.event`. Its optional `Disposable.step()` drives one nonblocking pipe slice
per Native Runtime pump; queue overflow, malformed pipe traffic, failed event
delivery, and unexpected child exit stop and reap the child. It does not silently
retry requests with unknown outcomes. Cancelling thread initialization also stops
the child before `thread/start`; cancelling an active turn requests an interrupt
and leaves the RPC outcome subject to the Session's unknown-result semantics.

Build the runtime-only candidate package before importing it into Ghosterm:

```sh
python3 -B scripts/build-plugin-codex.py --fetch
python3 -B scripts/build-plugin-codex.py --fetch --check
```

The output is `build/plugins/ai-codex/` (override with `--out`). It contains the
Client and Executor JavaScript dependency closure, manifest, argument and backend
protocol schemas, license, instructions, and deterministic integrity index. The
builder uses the locked TypeScript 5.9.3 toolchain, checks imports and schemas, and
exercises the compiled entries with the isolated fake Host / app-server fixture.
`--check` rebuilds in repository `tmp/` and compares every output byte. `--fetch`
permits fetching the integrity-locked developer tools; omit it for offline builds
when those tools are cached. Import the output directory through the existing
package review UI; source directories are not installation artifacts. Nothing is
automatically installed or trusted. See `PACKAGE.md` for mapping and consent.

The product UI renders bounded file paths, change kinds, moves, and colored diff lines,
and associates file approvals with the corresponding item. Oversized patches show
an explicit partial-preview or omitted-event notice. Complete diff review and the
product path against a live Codex turn and approval remain unverified. Validate
the Executor entry with an isolated fake Host and fake app-server (no model or
external service):

```sh
python3 -B scripts/test-plugin-codex-entry.py --fetch
python3 -B scripts/mutation-check-plugin-codex-entry.py
```

The lock was derived from the official Codex CLI `codex-cli 0.166.0` generators with experimental APIs disabled. Recheck the installed CLI and its generated JSON Schema and TypeScript surface with:

```sh
python3 scripts/check-plugin-codex-protocol.py
```

The checker also locks the complete stable decision unions used by command and
file-change approval responses. `accept`, `acceptForSession`, `decline`, and
`cancel` remain distinct; command approvals additionally include the two
structured policy-amendment decisions. A newer CLI decision is a compatibility
change that must be reviewed before updating the lock.

Structured command decisions preserve the official nested payloads:
`acceptWithExecpolicyAmendment.execpolicy_amendment` is a string array and
`applyNetworkPolicyAmendment.network_policy_amendment` carries the host/action
pair. Flat lookalike objects are rejected. The lock checker compares these
normalized generated TypeScript shapes and the nested JSON Schema wrappers,
not just union member names.

The pinned `0.166.0` process is exercised through the production
`spawnCodexAppServer` helper and a test Host `ProcessChild` adapter. This covers
the Host-reported version check, helper initialization, `initialized`,
`thread/start`, bounded pipe stepping, conversation close, and reap in one
isolated run. The
test Host probes `codex --version` separately and supplies that result as its
verified mapping; this does not prove product Host discovery or Core Runtime
`ctx.process` wiring. Its server replies omit the `jsonrpc` member, so the
pinned envelope behavior is recorded separately from generated payload types
and checked with the adapter:

```sh
node --experimental-strip-types --test scripts/test-plugin-codex-protocol.mjs
```

The protocol client initializes each connection once, sends `initialized`, and exposes the locked `thread/start`, `turn/start`, and `turn/interrupt` calls. The first turn input is text with the official `text_elements` field; the current `turn/start` helper exposes only thread ID and input rather than forwarding thread-level configuration. It preserves each server-initiated approval request id and validates the distinct stable command/file response shapes. `serverRequest/resolved` expires a pending approval; pipe closure rejects unfinished outbound calls as unknown without replay. `protocol/stdio.ts` composes that client and the bounded line decoder over a Host-injected nonblocking pipe surface. It limits each IO slice, bounds queued writes, and drains stderr without retaining it, including after stdout EOF. `protocol/process.ts` connects this adapter to the portable Host process contract; Core's QuickJS Runtime supplies that contract only when its checked Executor package, live grants, Host executable mapping, and authorized workspace root all agree. After natural child exit, the helper drains both pipes and automatically releases/reaps once both reach EOF; callers can still explicitly stop a running child. Once release completes, the helper refreshes its exit snapshot from the Host. Core Runtime also enforces a finite per-child lifetime and aggregate stdout/stderr read quota. The Codex helper compares the Host-reported verified version with the app-server lock before sending initialize; after initialization it also parses the server-reported version token from the required `InitializeResponse.userAgent` and rejects a mismatch before exposing `ready` to callers. Either mismatch terminates and reaps the child. The lock checker pins `InitializeResponse.userAgent` as a required string against the official generated TypeScript and JSON Schema. The production-helper test initializes and starts a thread with the pinned real CLI, checking its live version report; a separate fake-child test proves mismatch cleanup. Core checks the Host-supplied executable SHA-256 when Runtime opens and rejects path/file identity changes observed before spawn. These checks do not prove the product App Hosts supply a verified mapping, that exec is descriptor-pinned, or that a live Codex turn, approval UI, or GUI event delivery works.

The locked schema and conversation facade do not claim a real model turn or approval action. The conversation tests use isolated protocol events; the real CLI test stops after `thread/start`. Native approval buttons, activity views, and bounded patch previews are wired, but live product turn / approval round trips and complete diff review remain unverified P3 work. `codex.event` uses an object input with an `event` JSON string so it matches the Native / gterm object method registry; the complete encoded input must fit the Core 3072-byte RPC limit. See the [official Codex app-server protocol](https://learn.chatgpt.com/docs/app-server).
