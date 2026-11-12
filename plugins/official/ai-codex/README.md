# Official Codex adapter

The P3 protocol adapter is in `protocol/client.ts`; it handles initialization,
thread / turn request correlation, streamed notifications, approval request IDs,
and unknown outcomes on pipe closure. `protocol/line-decoder.ts` frames bounded
UTF-8 JSON lines without Node or DOM APIs. `protocol/process.ts` launches the
Host mapping named `codex` through `PluginContext.process` after checking the
Executor role, current `process.spawn` / `process.pipes` grants, and opaque
authorized workspace handle. It returns the adapter with explicit bounded
`step()` and `stop()` lifecycle. This is not yet an installable or complete
plugin; product Host mappings and UI are not wired.

The lock was derived from the official Codex CLI `codex-cli 0.166.0` generators with experimental APIs disabled. Recheck the installed CLI and its generated JSON Schema and TypeScript surface with:

```sh
python3 scripts/check-plugin-codex-protocol.py
```

The checker also locks the complete stable decision unions used by command and
file-change approval responses. `accept`, `acceptForSession`, `decline`, and
`cancel` remain distinct; command approvals additionally include the two
structured policy-amendment decisions. A newer CLI decision is a compatibility
change that must be reviewed before updating the lock.

The pinned `0.166.0` process was also exercised through a real stdio handshake.
Its server replies omit the `jsonrpc` member, so the pinned envelope behavior is
recorded separately from generated payload types and checked with the adapter:

```sh
node --experimental-strip-types --test scripts/test-plugin-codex-protocol.mjs
```

The protocol client initializes each connection once, sends `initialized`, and exposes the locked `thread/start`, `turn/start`, and `turn/interrupt` calls. It preserves each server-initiated approval request id and validates the distinct stable command/file response shapes. `serverRequest/resolved` expires a pending approval; pipe closure rejects unfinished outbound calls as unknown without replay. `protocol/stdio.ts` composes that client and the bounded line decoder over a Host-injected nonblocking pipe surface. It limits each IO slice, bounds queued writes, and drains stderr without retaining it, including after stdout EOF. `protocol/process.ts` connects this adapter to the portable Host process contract; Core's QuickJS Runtime supplies that contract only when its checked Executor package, live grants, Host executable mapping, and authorized workspace root all agree. After natural child exit, the helper drains both pipes and automatically releases/reaps once both reach EOF; callers can still explicitly stop a running child. Core Runtime also enforces a finite per-child lifetime and aggregate stdout/stderr read quota. Isolated tests cover the helper with a fake Host child and the production stdio adapter with the pinned real CLI separately. They do not yet prove the full Core Runtime launches the real Codex server, nor a live turn, approval UI, App Host process mapping, executable identity revalidation, or GUI event delivery.

The locked schema is only a compatibility baseline. It does not claim a live Codex turn, process pipe service, native chat / approval / diff views, or end-to-end Ghosterm integration. Those remain subsequent P3 work. See the [official Codex app-server protocol](https://learn.chatgpt.com/docs/app-server).
