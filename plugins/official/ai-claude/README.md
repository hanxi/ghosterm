# Claude Node backend component (candidate)

These are the initial P4 backend slices. This is **not an importable Ghosterm plugin**:
there is no `plugin.json`, gterm Node/Exec runtime, Native page, Ghosterm RPC method or
Native conversation adapter yet. The existing gterm and App profiles still advertise
Portable JS only. An unsupported Node variant must remain unsupported.

`node/` contains an authored MIT component with a separately locked dependency on
Claude Agent SDK **0.3.323** and its matching native CLI **2.1.323**. The supplied
process owner must verify the actual executable and live authorization; declaring
the version does not verify a file or grant access. An ordinary system CLI is not
silently substituted. SDK dependencies and the native CLI retain their own
licenses/terms, as recorded in `node/package-lock.json` and the installed packages;
they are not relicensed under this repository's MIT license or included in an App.

The prompt broker binds each SDK `requestId`, `toolUseID`, agent, input and MCP
provenance to a scope and a fresh opaque reference. Inputs and backend display
metadata are frozen JSON snapshots. Unknown MCP source strings remain unknown,
not `sdk`. A decision resolves once; identical duplicates share/replay the same
decision while retained. Changed identity or input under one ID fails the broker.
Cancellation and close reject the pending callback as expired, without creating
a user decline. Old references do not authorize a replacement broker, even if a
caller repeats its scope label. Nothing survives process death or reconnect.

Tool approval preserves the original input and never adopts permission-update
suggestions. `AskUserQuestion` requires explicit answers to every question; it
cannot be accepted by an ordinary allow button. Multi-select answers use the
SDK's string answer representation, and explicit free text is supported. Option
previews are not rendered. These are private Node objects, not frozen wire v1
messages or Native UI components.

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
There is no unsolicited output. Writes await completion before dispatching the
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
The standalone executor is not yet a gterm Node runtime or a shippable plugin.
An owned conformance fixture now launches it through a real native HOST Session,
gterm's Portable Supervisor and Host-fixed process arguments. Additional selected
environment names stay on the executing machine. The fixture verifies an exact
selected value and exclusion of an unselected parent marker, initializes the real
CLI without a model prompt, stops it and waits for direct-child cleanup. This is
not a production ai-claude package, generic Node package loader or Native chat UI.

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
