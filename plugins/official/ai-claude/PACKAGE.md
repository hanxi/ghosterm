# Claude SDK bridge candidate

This package contains a Portable Client/Executor supervisor and authored MIT code.
It requires an explicitly Host-configured program named `claude-node`, mapped to
Node >=20 with fixed arguments selecting the separately installed Node component
and owner profile. The Node component requires the locked Agent SDK 0.3.323 and
matching CLI 2.1.323, under their own terms; dependencies are not in this package.
Missing dependencies or invalid configuration fail without installation, identity
fallback or subprocess replay. The Host must review that external component.

The eight private RPC methods upload bounded JSON parameters, issue one explicit
child command, read complete immutable reply pages and release them. They retain
SDK-specific events and questions. Unknown outcomes stay unknown. A requested
start/interrupt is not a model result or an interrupt-completed receipt.
Inputs are compact JSON objects up to 64,000 UTF-8 bytes, uploaded once in small
chunks. Replies are retained up to 600,000 bytes each, under a 2 MiB/four-record
reservation budget allocated before effects. Page offsets count UTF-16 units;
read and forget complete receipts to release their budget. Status can expose an
observed receipt after RPC cancellation, without retrying the action. Explicit
stop waits for observed exit and Core process release.

macOS provides experimental Native conversation, approval and question controls
for this command, using a matching external Node component from this checkout.
Other Host UIs remain unfinished. This candidate advertises no portable Native
view contribution and does not implement generic runtime:node package loading. Normal
App builds keep plugins disabled. Real model/authentication/tool/device and
whole-process resource/lifecycle verification remain incomplete. This is a
development bridge, not a released Claude conversation product.
