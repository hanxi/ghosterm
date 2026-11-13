# Claude Node backend component (candidate)

This is the first P4 backend slice. It is **not an importable Ghosterm plugin**:
there is no `plugin.json`, gterm Node/Exec runtime, Native page, wire method or
model-session adapter yet. The existing gterm and App profiles still advertise
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
