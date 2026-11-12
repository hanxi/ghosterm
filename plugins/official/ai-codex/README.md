# Official Codex adapter

This directory starts the P3 Codex integration. The only current artifact is the stable app-server protocol lock in `protocol/app-server.lock.json`; this is not yet an installable or runnable plugin.

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
recorded separately from generated payload types and checked by the live probe.

The first adapter must initialize each app-server connection once, send the `initialized` notification, then use the locked `thread/start`, `turn/start`, and `turn/interrupt` requests. It must preserve the JSON-RPC request id for server-initiated command and file-change approval responses. Pending UI decisions are scoped by thread, turn, item, and request id; `serverRequest/resolved`, interruption, completion, process exit, and channel loss invalidate the pending decision. An unconfirmed decision stays unknown and is never replayed.

The locked schema is only a compatibility baseline. It does not claim a live Codex turn, process pipe service, native chat / approval / diff views, or end-to-end Ghosterm integration. Those remain subsequent P3 work. See the [official Codex app-server protocol](https://learn.chatgpt.com/docs/app-server).
