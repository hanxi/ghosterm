# Official Codex adapter

This directory starts the P3 Codex integration. The only current artifact is the stable app-server protocol lock in `protocol/app-server.lock.json`; this is not yet an installable or runnable plugin.

The lock was derived from the official Codex CLI `codex-cli 0.166.0` generators with experimental APIs disabled. Recheck the installed CLI and its generated JSON Schema and TypeScript surface with:

```sh
python3 scripts/check-plugin-codex-protocol.py
```

The first adapter must initialize each app-server connection once, send the `initialized` notification, then use the locked `thread/start`, `turn/start`, and `turn/interrupt` requests. It must preserve the JSON-RPC request id for server-initiated command and file-change approval responses. Pending UI decisions are scoped by thread, turn, item, and request id; `serverRequest/resolved`, interruption, completion, process exit, and channel loss invalidate the pending decision. An unconfirmed decision stays unknown and is never replayed.

The locked schema is only a compatibility baseline. It does not claim a live Codex turn, process pipe service, native chat / approval / diff views, or end-to-end Ghosterm integration. Those remain subsequent P3 work. See the [official Codex app-server protocol](https://learn.chatgpt.com/docs/app-server).
