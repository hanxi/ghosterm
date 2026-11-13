# Ghosterm Codex package

This package runs the Codex adapter through the user-selected remote gterm Executor.
It requires Ghosterm SDK 1.x / bridge v1 and codex-cli 0.166.0 installed by the user.
It does not install Codex, copy credentials, or select an executable via PATH.

Import this directory with the Ghosterm plugin package review flow. Review its
SHA-256 and explicitly approve the Client package. Place the same checked package
on the selected Executor host and select its integrity digest separately. Launch
`ai codex` with no arguments, an explicit workspace root, and a `codex` process
mapping containing the absolute executable path, expected version
`codex-cli 0.166.0`, and executable SHA-256. Consent grants session.rpc to the
Client and Executor and process.spawn / process.pipes to the Executor only.
The adapter verifies both the Host version declaration and initialize userAgent.
Missing mappings or a version mismatch fail explicitly.

Include these Executor methods in the reviewed RPC allowlist:
`codex.thread.start`, `codex.turn.send`, `codex.turn.interrupt`,
`codex.approval.respond`, and `codex.change.read`. The Native Host method is
`codex.event`. Adding the read method grants no filesystem capability; it reads
only bounded in-memory patches already received from this Codex child.

The Client has no JavaScript event handler: supported Native Hosts receive
codex.event and provide thread, turn, interrupt and approval controls. Events use
an object with an event JSON string. Diff previews are bounded and may be partial;
the UI identifies truncation. Unknown effects are never automatically replayed.
The Executor also exposes complete patch snapshots through `codex.change.read`.
Supported Native Hosts require a validated current complete document before
enabling file approval. `codex.approval.respond` now requires all three fields:
`requestID`, `decisionJSON`, and `snapshotID`. File acceptance sends the snapshot
ID captured at the button click; the Executor verifies its current captured
thread/turn/item revision synchronously before enqueueing the Codex response.
Command decisions and file decline/cancel send an empty snapshot ID. Stale,
evicted, oversized or missing revisions cannot authorize file acceptance;
decline remains available. This candidate business RPC shape replaces the old
two-field input; old callers fail schema validation. No wire v1 or CLI stable
schema changes. Reading a complete patch does not prove human review, and this
check does not compare the workspace's current filesystem bytes.
Revision mismatches return `accepted: false`: the RPC reply is confirmed, but no
approval was written. Unclassified handler exceptions and transport loss still
remain unknown; this does not turn interrupted work into a rejection.
Stopping the Runtime closes the protocol and terminates/reaps the owned child.

This is an unsigned candidate package. Integrity detects changed content; it does
not authenticate its publisher or authorize execution. Compiled-package fixture
checks do not prove a live model turn, real approval round trip, Android device
operation, or a complete user interaction flow.
