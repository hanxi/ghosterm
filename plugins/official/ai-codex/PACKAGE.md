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
The Executor also exposes complete patch snapshots through `codex.change.read`;
Native full-patch review controls are still being implemented. Existing partial
previews do not prove that a user has reviewed the complete change.
Stopping the Runtime closes the protocol and terminates/reaps the owned child.

This is an unsigned candidate package. Integrity detects changed content; it does
not authenticate its publisher or authorize execution. Compiled-package fixture
checks do not prove a live model turn, real approval round trip, Android device
operation, or a complete user interaction flow.
