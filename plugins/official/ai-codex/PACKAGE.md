# Ghosterm Codex package

This package runs the Codex adapter through the user-selected remote gterm Executor.
It requires Ghosterm SDK 1.x / bridge v1 and codex-cli 0.166.0 installed by the user.
It does not install Codex, copy credentials, or select an executable via PATH.

Import this directory with the Ghosterm plugin package review flow. Review its
SHA-256 and explicitly approve the Client package. Place the same checked package
on the selected Executor host and select its integrity digest separately. Launch
`ai codex` with an explicit workspace root and a `codex` process
mapping containing the absolute executable path, expected version
`codex-cli 0.166.0`, and executable SHA-256. Consent grants session.rpc to the
Client and Executor and process.spawn / process.pipes to the Executor only.
The adapter verifies both the Host version declaration and initialize userAgent.
Missing mappings or a version mismatch fail explicitly.

Optional flags are `--ephemeral` (ask the backend not to persist this thread) and
`--read-only` (request a read-only sandbox with on-request decisions reviewed by
the user). With no flags the backend retains its configured defaults. The
read-only mode requires the actual thread/start response to confirm all three
policy fields before accepting a turn. Ephemeral mode does not promise that the
CLI or provider retains no operational logs. Unknown flags fail before spawning.

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
Unsupported backend requests produce a terminal protocol error before the owned
child closes; they do not leave the native conversation silently busy.


Stable `item/permissions/requestApproval` is a separate approval class. Native
Hosts display the complete requested filesystem/network overlay and label the
decision for the current turn. Allow repeats that exact normalized overlay;
deny sends an empty overlay. Neither response is a command/file decision
wrapper. No session grant or review-policy override is sent. Unsupported scope
fields or special aliases fail explicitly; oversized approvals are not truncated
into an approvable request. This does not grant Ghosterm local file/network
capabilities or change the backend's other restrictions.

Every permission event carries an opaque `permissionReviewID`. Both allow and
deny must echo that current reference in `codex.approval.respond.snapshotID`;
a stale reference returns `accepted: false` before a child write. A reused vendor
request ID receives a fresh reference. File decisions retain their existing
complete-patch revision checks; command decisions continue to use an empty
snapshot ID. Optional nullable wire fields are normalized to their documented
empty/default meaning without expanding paths or folding Unicode.

The pinned CLI exposes this stable RPC, while its model-facing
`request_permissions_tool` feature is experimental and disabled by default.
This package does not enable it. Select that backend feature explicitly when
needed; ordinary file/command approval does not depend on it.

This is an unsigned candidate package. Integrity detects changed content; it does
not authenticate its publisher or authorize execution. Compiled-package fixture
checks do not prove a live model turn, real approval round trip, Android device
operation, or a complete user interaction flow.
