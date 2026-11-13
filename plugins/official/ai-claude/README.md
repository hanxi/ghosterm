# Claude SDK bridge (candidate)

The Portable Client/Executor candidate builds into a directory package with
`plugin.json`, integrity metadata and eight private RPC methods. Its supervisor
starts the separately configured Node component only on an explicit `start`.
macOS and Android have experimental Native conversation, tool approval and
question controls for this command. Other Hosts and generic Node/Exec package loading remain
unfinished; the candidate declares no portable Native view contribution. The App
and gterm package profiles continue to support Portable JS only.

`node/` contains an authored MIT component with a separately locked dependency on
Claude Agent SDK **0.3.323** and its matching native CLI **2.1.323**. The supplied
process owner must verify the actual executable and live authorization; declaring
the version does not verify a file or grant access. An ordinary system CLI is not
silently substituted. SDK dependencies and the native CLI retain their own
licenses/terms, as recorded in `node/package-lock.json` and the installed packages;
they are not relicensed under this repository's MIT license or included in an App.
The selected dependency closure requires **Node >=20** (`@hono/node-server`),
although the SDK's own minimum is 18.14. The component and supervisor reject an
older declared Node version. The observed test version is Node 26.4.0.

The prompt broker binds each SDK `requestId`, `toolUseID`, agent, input and MCP
provenance to a scope, the locally active input UUID and a fresh opaque reference. Inputs and backend display
metadata are frozen JSON snapshots. Unknown MCP source strings remain unknown,
not `sdk`. A decision resolves once; identical duplicates share/replay the same
decision while retained in the same turn. Reusing an ID in another explicit turn,
or changing identity/input under that ID, fails the broker. This local binding
does not prove the source turn of every possible late backend callback.
Cancellation and close reject the pending callback as expired, without creating
a user decline. Old references do not authorize a replacement broker, even if a
caller repeats its scope label. Nothing survives process death or reconnect.

Tool approval preserves the original input and never adopts permission-update
suggestions. `AskUserQuestion` requires explicit answers to every question; it
cannot be accepted by an ordinary allow button. Multi-select answers use the
SDK's string answer representation, and explicit free text is supported. Option
previews are not rendered. These remain private Node objects, not frozen wire v1
messages; macOS and Android project them into their own Native controls.

Limits: 32 pending / 64 retained requests, one MiB of retained serialized request
identity, 64 KiB tool input, 32 JSON levels / 4096 nodes, eight pending duplicate
signals and 128 queued events. An unread request event keeps its payload charged
after a decision; it cannot be evicted to bypass the budget. Closed brokers clear
their reservations. These are component limits, not an SDK/CLI process RSS cap.

The explicit profile selects cwd, executable, environment, tools and process
owner; it rechecks scope/abort state and execution target at spawn, and refuses
process replay even if options are rebuilt for the same broker. It disables
filesystem setting sources, implicit MCP configuration, plugins and session
transcript persistence. It retains the backend's `default` permission mode.
`canUseTool` only receives requests the backend asks about; this is not a promise
that every tool action prompts, and it is not an OS sandbox. See the official
[TypeScript API](https://code.claude.com/docs/en/agent-sdk/typescript#canusetool)
and [approval flow](https://code.claude.com/docs/en/agent-sdk/user-input).

The Node compiler uses DOM declarations because the pinned MCP peer exposes the
standard fetch `HeadersInit` type. This introduces no browser service or UI;
`packages/plugin-sdk/portable` and Core remain unchanged and do not import Node
or DOM. SDK types are checked with `skipLibCheck: false`.

`ClaudeNodeSession` now connects this profile and broker to the locked SDK's
streaming input and output. It accepts one explicit input at a time and returns
its SDK UUID as a correlation key, without claiming delivery. Backend messages
remain immutable, ordered vendor records; result errors and optional diagnostics
are preserved rather than converted into Codex events. A foreign or absent result
UUID ends observation as lost, while keeping the diagnostic and unresolved input.
Interrupt expires pending prompts immediately and shares one control request;
its receipt does not complete the turn. A correlated result retires any remaining
prompts before another input can be accepted. Close revokes the whole broker.

Session limits are 256 unread events / two MiB queued bytes, 512 KiB per SDK
message, 64 MiB cumulative serialized output and ten minutes from startup, with
explicit lower limits available. One event slot / 512 bytes is reserved for the
terminal observation. The SDK may keep waiting after stdout EOF in streaming
mode, so the adapter also observes its owned process and permits up to 500 ms for
buffered frames to drain. EOF remains a loss observation; an already-received
correlated result is preserved. SDK close/abort requests do not prove OS reaping:
the embedding owner must still observe exit and enforce its child/pipeline
limits. These counters do not cover SDK internal queues or process RSS.

The compiled `node/dist/cli.js` is now a foreground **standalone Node executor**:

```sh
node node/dist/cli.js --profile /explicit/owner/profile.json
```

It needs the separately installed, locked dependency closure. It does not install
anything. The profile is a bounded, current-user-owned regular file opened without
following its final symlink. It has exactly these fields:

```json
{"version":1,"cwd":"/explicit/workspace","executable":"/explicit/claude","cliVersion":"2.1.323","sha256":"<64 lowercase hex characters>","environmentNames":["HOME","CLAUDE_CONFIG_DIR","TMPDIR","PATH"],"tools":[]}
```

The launcher supplies the listed environment variables; no value is guessed or
read from a wire request. Do not put credentials in this file. Authentication
variables, if needed later, must be explicitly selected from the executing
machine's environment and use a supported SDK authentication method. Missing
variables fail rather than falling back to another HOME or identity. The executor
checks the CLI file SHA-256, locked SDK metadata and the real CLI `--version`, then
rechecks executable consistency immediately before SDK spawn. This does not
eliminate same-user filesystem races or validate the whole installed dependency
tree. The profile does not prove a Core grant or provide an OS sandbox.

The private stdio protocol uses UTF-8 newline-delimited canonical JSON (sorted
object keys, no whitespace/duplicate keys, JSON.stringify-style strings/numbers),
with 64 KiB inbound frames. A request is `{version:1,id,sequence,method,params}`.
Sequences start at one and increase by one. An exact retained duplicate returns
the same reply; changed/evicted sequences fail, never replaying effects. At most
32 replies / two MiB of serialized requests and replies are cached after each
response; one in-flight request/response has separate bounded temporary storage.
The high-water sequence is preserved after cache eviction.
Responses include both id and sequence, plus `ok` and result/error. A parent must
correlate both fields and wait for its response before issuing the next command.

Methods: `start`, `send` (`text`), `interrupt`, `approve` (`reference`, boolean
`allow`), `answer` (`reference`, `answers`), `poll` and `stop`. Unmentioned params
are rejected. Start/interrupt reply with `requested: true`; readiness and actual
interrupt receipts remain separate poll observations. Ready proves SDK control
initialization, not usable credentials or a successful model request. Poll fairly takes one
session/approval event, or returns the current phase without inventing an event.
An optional `poll` parameter `{maxEvents: N}` (integer 1..32) requests a bounded
`source: "batch"` response. It takes complete events while the accumulated prefix
is below 4 KiB, including the full last event even if that crosses the prefix
limit. Default empty params retain the single-event shape. Rebuild the external
Node component from the matching checkout before using the macOS controls; an
older component rejects the new params. There is no unsolicited output. Writes await completion before dispatching the
next command; framing rejects malformed UTF-8 and partial EOF. Protocol requests
cannot change executable/cwd/environment or trigger an installation.

The executor closes its SDK scope on EOF, error, signal, explicit stop or its
finite deadline. It drains/discards child stderr and requests cancellation once
the cumulative count exceeds 16 MiB (in-flight bytes can exceed that threshold), observes its
owned CLI exit, and escalates TERM to KILL with bounded waits if necessary.
Exit 0 means explicit stop and observed direct-child cleanup, not business
success; 7 is transport EOF, 130 cancellation, 4 missing dependency, 2 invalid
profile/invocation and 5 protocol/runtime/cleanup failure. SDK descendants,
whole-process RSS and the complete gterm process-group lifecycle remain unverified.
The candidate package launches this executor through a Host program mapping named
`claude-node`: Node executable/version/hash, fixed entry and `--profile` arguments,
workspace root and selected environment names. It neither installs dependencies
nor infers credentials. The external entry/profile/dependency tree is selected by
the Host and is not captured inside the Portable package integrity index.

RPC methods are `claude.input.open/append/seal/discard`, `claude.call`,
`claude.response.read/forget` and `claude.status`. Parameters are compact JSON
objects, uploaded in offset-checked chunks and consumed once; the final object is
limited to 64,000 UTF-8 bytes. Offsets and lengths count UTF-16 units. Replies keep
the complete child record (up to 600,000 bytes) in immutable pages of at most 256
UTF-16 units, without splitting a surrogate pair. Before any child action, reserve
its maximum reply size under a 2 MiB / four-document limit. Callers must read and
forget receipts to free this budget. A cancelled RPC can leave an observed receipt
available through status; this never changes its old outcome or authorizes replay.
Only one child operation is in flight. There is no implicit respawn. Stop succeeds
only after observing a zero exit without a signal and completing Core release.

Build the development import directory (no SDK/CLI binaries or `node_modules`):

```sh
python3 -B scripts/build-plugin-claude.py --fetch
python3 -B scripts/build-plugin-claude.py --check
```

The default output is `build/plugins/ai-claude`. Package tests exercise the compiled
entry against a named private-pipe fixture. The native integration runner
`scripts/test-plugin-claude-native.py --build <gterm-build>` additionally uses the
compiled production package over native HOST Session consent and gterm. Its two
separate modes use the real pinned SDK with an in-memory CLI protocol fixture for
large-input approval/question round trips, and the real matching CLI for no-model
initialization/stop/reap. Neither is evidence of a model turn or Native chat UI.

The macOS development launch form now accepts optional `prefixArguments` on each
process mapping and a separate JSON array of executing-machine environment names.
Names select values on that machine; they never copy values from the client.
The launch command still has the existing SSH route length bound. The Native
`AppModel.requestClaudeAction` adapter requires the current active Client Runtime
and all eight RPC methods. It uploads exact compact parameter JSON and returns
complete vendor reply bytes, preserving distinct Unicode spellings. After an
uncertain call it refuses further effects in that run. macOS offers explicit read-only observation of a retained receipt after an
uncertain operation. It binds the original input/response and command identity,
keeps the original outcome unknown, and never resends or resumes the conversation.
The Android observation UI remains unfinished. The macOS and Android conversations use
this adapter for explicit start/send/interrupt/stop, immutable tool approvals and
all-question answers. A ready backend is not proof of authentication or a model
result. The controls preserve unknown outcomes and original draft text after an
unconfirmed send, reject stale turn references and do not retry automatically.
Each visible/history collection retains at most 128 records and 2 MiB, with
omission counts and selectable complete content for retained records. Android
also has the matching fixed-argument/environment-name launch form and ACTIVE
Runtime complete-document adapter. Other Host adapters, real authentication and
model turns remain unfinished.

Android document/launch checks use the prepared local Android toolchain and an
owned no-window AVD; they make no SDK/CLI/model request:

```sh
python3 -B scripts/test-plugin-claude-android.py --headless-api 36 --conversation --mutate
python3 -B scripts/test-plugin-claude-android.py --headless-api 29 --conversation --plugin-js
```

The Android Native SDK runner requires already built JS-enabled Debug and test
APKs. It creates an owned no-window AVD and uses the same explicit CLI fixture:

```sh
python3 -B scripts/test-plugin-claude-sdk-android.py --headless-api 29 --gterm <JS-enabled-gterm> --route all --fetch
```

`--route all` checks direct SSH, ProxyJump and repeated checked-package argument
decoding. It uses real Settings controls for start/send/tool approval/questions
and stop; package approval and launch configuration are injected. It neither
executes an actual model/tool nor proves the complete installation flow.

The offscreen macOS integration runner is:

```sh
python3 -B scripts/test-plugin-claude-app.py --gterm <JS-enabled-gterm> --route both --fetch
python3 -B scripts/mutation-check-claude-conversation.py
```

The UI runner uses owned loopback sshd targets, actual production Native controls,
Portable package and locked SDK with a named in-memory CLI protocol fixture. It
clicks tool approval and all four question choices, then stops and observes reap,
over direct SSH and ProxyJump. Package approval and text draft setup are injected;
this does not test the full installation/input flow. It sends no real model
request or real tool command and supplies no personal account. Whole-process RSS,
sustained model streaming/backpressure, real authentication and device matrices
remain unverified. The mutation runner requires the ordinary JS=OFF Core profile.

From the repository root:

```sh
python3 -B scripts/test-plugin-claude-node.py --fetch --mutate --real-cli
# Subsequent runs use the same locked cache offline:
python3 -B scripts/test-plugin-claude-node.py --mutate --real-cli
```

The runner installs the integrity-locked closure into project `tmp/` with scripts
disabled, compiles the actual sources, and executes the emitted modules. Tests
use the real SDK against a named, in-memory CLI protocol fixture for permission,
cancel and question callbacks. The optional `--real-cli` probe is macOS-only:
it starts the matching native CLI under a no-network/securityd-denied test
profile, completes SDK initialization, sends no user prompt, and observes exit.
HOME/config/cwd are owned temporary directories; no personal login is selected.
It does not establish a model turn, real tool execution, vendor sandbox coverage,
SSH transport, Native approval UI, session resume or a shippable Node plugin.
