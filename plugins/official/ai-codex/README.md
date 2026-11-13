# Official Codex adapter

The P3 protocol adapter is in `protocol/client.ts`; it handles initialization,
thread / turn request correlation, streamed notifications, approval request IDs,
and unknown outcomes on pipe closure. `protocol/conversation.ts` layers one-thread
state, ordered event sequence, active-turn interrupt, approval expiry, and
unknown-on-close behavior over the client. `protocol/line-decoder.ts` frames bounded
UTF-8 JSON lines without Node or DOM APIs. `protocol/process.ts` launches the
Host mapping named `codex` through `PluginContext.process` after checking the
Executor role, current `process.spawn` / `process.pipes` grants, and opaque
authorized workspace handle. It returns the adapter with explicit bounded
`step()` and `stop()` lifecycle. macOS and Android product Hosts now register the
Codex RPC allowlist, receive bounded `codex.event` messages, and provide native
thread / turn controls, approval decisions, activity timelines, and raw-event
inspection. Launch still requires the user to grant a workspace and configure the
`codex` executable mapping with its expected version and SHA-256.

`executor/index.ts` is the current Native-independent Executor entry. It lazily
spawns only after `codex.thread.start`, exposes typed thread / turn / interrupt /
approval RPC handlers, and returns bounded ordered app-server events through
`codex.event`. Its optional `Disposable.step()` drives one nonblocking pipe slice
per Native Runtime pump; queue overflow, malformed pipe traffic, failed event
delivery, and unexpected child exit stop and reap the child. It does not silently
retry requests with unknown outcomes. Cancelling thread initialization also stops
the child before `thread/start`; cancelling an active turn requests an interrupt
and leaves the RPC outcome subject to the Session's unknown-result semantics.

Build the runtime-only candidate package before importing it into Ghosterm:

```sh
python3 -B scripts/build-plugin-codex.py --fetch
python3 -B scripts/build-plugin-codex.py --fetch --check
```

The output is `build/plugins/ai-codex/` (override with `--out`). It contains the
Client and Executor JavaScript dependency closure, manifest, argument and backend
protocol schemas, license, instructions, and deterministic integrity index. The
builder uses the locked TypeScript 5.9.3 toolchain, checks imports and schemas, and
exercises the compiled entries with the isolated fake Host / app-server fixture.
`--check` rebuilds in repository `tmp/` and compares every output byte. `--fetch`
permits fetching the integrity-locked developer tools; omit it for offline builds
when those tools are cached. Import the output directory through the existing
package review UI; source directories are not installation artifacts. Nothing is
automatically installed or trusted. See `PACKAGE.md` for mapping and consent.

The product UI renders bounded file paths, change kinds, moves, and colored diff lines,
and associates file approvals with the corresponding item. Oversized patches show
an explicit partial-preview or omitted-event notice. Native complete readers and
revision-bound approvals have component evidence; their product path against a
live Codex turn and approval remains unverified. Validate
the Executor entry with an isolated fake Host and fake app-server (no model or
external service):

```sh
python3 -B scripts/test-plugin-codex-entry.py --fetch
python3 -B scripts/mutation-check-plugin-codex-entry.py
python3 -B scripts/mutation-check-plugin-codex-expiry.py --apple
python3 -B scripts/mutation-check-plugin-codex-changes.py
```

Turn completion expires unanswered approvals for that thread and turn even when
the backend sends no separate `serverRequest/resolved` notification. The
conversation refuses approvals outside its active turn, and the Executor drops
its corresponding request handles. Native Hosts remove approval buttons on turn
completion, protocol failure, unknown outcomes, and closure; runtime failures also
invalidate pending buttons while preserving uncertain turn results. Expiry does
not send an approval or invent a tool outcome. The expiry mutation runner uses
disposable source copies, strict TypeScript checking, and optional real Swift
compilation to verify assertion failures, independent controls, restoration, and
source digests. Its locked tools use the package builder's cache; add `--fetch`
when that cache is empty.

`executor/changes.ts` retains complete backend change documents independently of
the preview limit. A document contains schemaVersion=1, threadId, turnId, itemId,
and the original changes array. Its preview `_ghosterm` marker carries snapshotID,
snapshotUnits and snapshotBytes, including when paths or other fields leave no
room for a partial diff. `codex.change.read({snapshotID, offset})` returns status,
the same ID, offset/nextOffset, totalUnits/totalBytes, content and done. Offsets
count UTF-16 code units, never split a surrogate pair, and every success outcome
fits the frozen 3072-byte RPC body budget. Only status=ok with done=true denotes
the end of complete content; expired/invalid statuses contain no data and are not
completion or approval. Session loss still yields the channel's unknown outcome.

Each snapshot is at most 512 KiB of serialized UTF-8 JSON. The store retains at
most 16 items and 1 MiB total, evicts oldest entries, and replaces a changed
item's ID so readers cannot mix revisions. Identical documents keep their ID;
oversized updates invalidate old content and advertise snapshotUnavailable=limit.
Stopping the Executor clears all snapshots. IDs belong to that Runtime instance
and must never be reused across Native Session caches. This is the complete-read
backend contract. Native complete readers and the revision binding described
below are implemented; their positive Agent-to-RPC-to-UI path remains unverified.
No extra resource or scoped-file permission is added.

The lock was derived from the official Codex CLI `codex-cli 0.166.0` generators with experimental APIs disabled. Recheck the installed CLI and its generated JSON Schema and TypeScript surface with:

```sh
python3 scripts/check-plugin-codex-protocol.py
```

The checker also locks the complete stable decision unions used by command and
file-change approval responses. `accept`, `acceptForSession`, `decline`, and
`cancel` remain distinct; command approvals additionally include the two
structured policy-amendment decisions. A newer CLI decision is a compatibility
change that must be reviewed before updating the lock.

Structured command decisions preserve the official nested payloads:
`acceptWithExecpolicyAmendment.execpolicy_amendment` is a string array and
`applyNetworkPolicyAmendment.network_policy_amendment` carries the host/action
pair. Flat lookalike objects are rejected. The lock checker compares these
normalized generated TypeScript shapes and the nested JSON Schema wrappers,
not just union member names.

The pinned `0.166.0` process is exercised through the production
`spawnCodexAppServer` helper and a test Host `ProcessChild` adapter. This covers
the Host-reported version check, helper initialization, `initialized`,
`thread/start`, bounded pipe stepping, conversation close, and reap in one
isolated run. The
test Host probes `codex --version` separately and supplies that result as its
verified mapping; this does not prove product Host discovery or Core Runtime
`ctx.process` wiring. Its server replies omit the `jsonrpc` member, so the
pinned envelope behavior is recorded separately from generated payload types
and checked with the adapter:

```sh
node --experimental-strip-types --test scripts/test-plugin-codex-protocol.mjs
```

The protocol client initializes each connection once, sends `initialized`, and exposes the locked `thread/start`, `turn/start`, and `turn/interrupt` calls. The first turn input is text with the official `text_elements` field; the current `turn/start` helper exposes only thread ID and input rather than forwarding thread-level configuration. It preserves each server-initiated approval request id and validates the distinct stable command/file response shapes. `serverRequest/resolved` expires a pending approval; pipe closure rejects unfinished outbound calls as unknown without replay. `protocol/stdio.ts` composes that client and the bounded line decoder over a Host-injected nonblocking pipe surface. It limits each IO slice, bounds queued writes, and drains stderr without retaining it, including after stdout EOF. `protocol/process.ts` passes complete POSIX argv (`["codex", "app-server"]`, including argv[0]) to the portable Host process contract; Core's QuickJS Runtime supplies that contract only when its checked Executor package, live grants, Host executable mapping, and authorized workspace root all agree. After natural child exit, the helper drains both pipes and automatically releases/reaps once both reach EOF; callers can still explicitly stop a running child. Before release, the helper retains the last exit snapshot; after release it never polls the invalidated Native child handle. A previously running child is known to have exited/reaped, but its final code and signal remain unknown (`null`) unless observed beforehand. Core Runtime also enforces a finite per-child lifetime and aggregate stdout/stderr read quota. The Codex helper compares the Host-reported verified version with the app-server lock before sending initialize; after initialization it also parses the server-reported version token from the required `InitializeResponse.userAgent` and rejects a mismatch before exposing `ready` to callers. Either mismatch terminates and reaps the child. The lock checker pins `InitializeResponse.userAgent` as a required string against the official generated TypeScript and JSON Schema. The production-helper test initializes and starts a thread with the pinned real CLI, checking its live version report; a separate fake-child test proves mismatch cleanup. Core checks the Host-supplied executable SHA-256 when Runtime opens and rejects path/file identity changes observed before spawn. These checks do not prove the product App Hosts supply a verified mapping, that exec is descriptor-pinned, or that a live Codex turn, approval UI, or GUI event delivery works.

The locked schema and conversation facade do not claim a real model turn or approval action. The conversation tests use isolated protocol events; the real CLI test stops after `thread/start`. Native approval buttons, activity views, and bounded patch previews are wired, but live product turn / approval round trips and complete diff review remain unverified P3 work. `codex.event` uses an object input with an `event` JSON string so it matches the Native / gterm object method registry; the complete encoded input must fit the Core 3072-byte RPC limit. See the [official Codex app-server protocol](https://learn.chatgpt.com/docs/app-server).


Exercise the built package with real Native Client / Executor Runtimes and the
pinned local CLI (initialize and thread/start only; no model request):

```sh
cmake -S cli/gterm -B tmp/codex-gterm -DGHOSTERM_GTERM_TESTS=ON -DGHOSTERM_WITH_PLUGIN_JS=ON -DGHOSTERM_WITH_LIBSSH2=OFF
cmake --build tmp/codex-gterm --target gterm test_gterm_codex
python3 -B scripts/test-plugin-codex-runtime.py --build tmp/codex-gterm
```

The fixture supplies an empty temporary HOME and workspace, checks the CLI version
and executable digest, explicitly consents to the Session, and receives the real
thread result and thread/started event. Native Runtime close tears down the route;
the gterm peer reports channel loss and still must complete owner cleanup. This
stdio fixture does not prove SSH or product UI behavior. gterm forwards only its
selected HOME, PATH, XDG_CONFIG_HOME, CODEX_HOME and TMPDIR values to children.

On macOS, exercise the existing Native AppModel flow over the gate's owned
loopback sshd with the same built package and CLI:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer python3 -B scripts/test-plugin-codex-app.py --gterm tmp/codex-gterm/gterm
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer python3 -B scripts/test-plugin-codex-app.py --gterm tmp/codex-gterm/gterm --route both
```

This explicitly reviews/approves the package, waits for Session consent, starts a
real thread, validates the actual thread/started notification in AppModel, then
stops and releases the run. The remote launcher supplies a fresh HOME / CODEX_HOME
and workspace before invoking gterm, so the fixture does not use the account's
Codex configuration. `--route both` also creates a second loopback sshd with its
own host key. The jump permits forwarding only to the target and forbids remote
command sessions. The additional cases verify both host-key prompts and a real
Codex thread through ProxyJump, and reject the jump host key before the Executor
launcher can run. `--route jump` runs just those two cases. The runner requires
the exact selected test count with no skips, and ignores inherited SSH fixture
endpoints. Both daemons are stopped and waited for before their files are removed.
No model turn is sent. This is AppModel integration evidence;
import dialogs, buttons, tmux, device behavior and model approvals still
need their own acceptance evidence. RPC-only sessions negotiate requests without
resource transfer, and a process cwd alone grants no scoped file-read capability.


macOS and Android now preserve full snapshot references even when the preview has no room
for any file, and offers `读取完整补丁` in timeline and file-approval rows. It
assembles contiguous UTF-16 chunks, verifies total UTF-8 bytes and exact
thread/turn/item identity, then decodes the complete document on a worker. Only a
validated document appears in the native file selector and read-only text view;
its full lines remain selectable and copyable. One complete document is retained
per AppModel, with at most 512 KiB serialized input, 8192 files, and 16 KiB per
path. Cancellation, replacement, unavailable runtime, and terminal failure clear
the review; late callbacks cannot repopulate another review. Reads have an
aggregate publication deadline and no automatic retry. Reading a complete patch
does not prove human review. File acceptance now carries the exact snapshot ID
captured when its button was clicked; Native policy requires a complete current
document with the approval's exact thread/turn/item identity. The Executor checks
the current captured revision synchronously before enqueueing its response, and
rejects stale, unrelated, evicted or unavailable snapshots. It does not compare
the actual workspace filesystem bytes. `codex.approval.respond` requires
`requestID`, `decisionJSON`, and `snapshotID`; command decisions and file
decline/cancel send an empty snapshot ID. This replaces the candidate two-field
input, so old callers receive a schema error. A revision mismatch returns the
confirmed business result `accepted: false` without writing an approval;
unclassified handler errors and interrupted transport remain unknown. The
macOS and Android full-patch RPC-to-UI fixtures are described below; real model
approval and the complete installation/interaction/device matrix remain pending. The Android native reader uses strict
streaming JSON, exact Kotlin string identity, a selectable read-only Compose
text field, and the same publication limits. Default App builds still have the
optional plugin JS runtime disabled.

Run the macOS component tests and isolated eight-group mutations against the
default Core build (the first command configures that matching profile):

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer GHOSTERM_APP_TEST_FILTER=CodexChangeReviewTests bash scripts/apple-app-gate.sh
python3 -B scripts/mutation-check-plugin-codex-review.py
```

Android assembly and cancellation can also run on the host JVM using the cached
Android Kotlin compiler; this does not exercise Android JSON or Compose:

```sh
python3 -B scripts/test-android-codex-review.py --mutate
```

For the explicit test device, the following builds and installs JS-enabled Debug
and test APKs, proves three single-assertion failures after real compilation,
checks the same APK's independent Core control, and restores source SHA-256:

```sh
ANDROID_SERIAL=emulator-5586 python3 -B scripts/mutation-check-android-codex-review.py
```

The JSON, timeline, Compose and Core combination passed 17 cases on each of the
disposable API 29 and API 36 arm64 / 4 KiB headless emulators. Component fixtures
alone do not replace real Agent requests or the complete installation-to-review
user flow; the separate protocol-fixture RPC tests are described below.

The AppModel SSH runner's direct selection now also holds a disposable wrapper
at a FIFO before exec of Codex, observes its actual start, stops that pending
thread request, and establishes a replacement Session with the real CLI. Thus
`--route direct` requires two passing cases and `--route both` requires four;
`--route jump` still requires two. This does not claim a real model turn or
verified human review of an approval.

The separate patch runner uses a clearly identified Python app-server protocol
fixture, not the official CLI or a model. It sends two complete 166178-byte
documents through real owned SSH / gterm / QuickJS / Native RPC, each containing
ten files, a 4096-character path component and 600 Unicode diff lines per file.
The first path forces every preview file to be omitted. On both direct and
ProxyJump routes, the production Extensions pane receives local AppKit mouse
events for consent, thread start, send, full reads, approval and stop. It checks
the complete readonly NSTextView text, disabled unread approvals, replacement
invalidation, confirmed stale-version refusal, and the original approval ID and
decision at the child. Package review/start and message draft setup use AppModel
APIs; this does not cover installation UI or keyboard typing. Rendering uses a
transparent offscreen window with controlled occlusion state, not hardware focus
or accessibility validation. The fixture never applies a patch or calls a model.

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer python3 -B scripts/test-plugin-codex-patch-app.py --gterm tmp/codex-gterm/gterm --route both
python3 -B scripts/mutation-check-plugin-codex-stdio.py
```

The stdio owner separates framing/UTF-8 decoding, JSON validation and business
dispatch into successive slices, retains at most 32 decoded lines and one
single-use prepared message, and continues draining stderr during both later
phases. Closing drops the prepared message; its closure also refuses delivery to
a closed client. UTF-8 is decoded in blocks of at most 8192 bytes, carrying an
incomplete code point across blocks while retaining the whole line's one MiB
byte/EOF limits. Exact UTF-8 counts use native regexp passes; all 1112064 Unicode
scalars, malformed surrogate inputs and mixed full documents are checked against
Node's TextEncoder reference. This keeps the fixed CPU budget; it does not
establish the full maximum-size or cold-start performance matrix. Exact RPC byte
checks remain after inexpensive preview pruning. The Portable SDK is unchanged.

The Android counterpart uses the production MainActivity Settings and
ExtensionPackagesViewModel. It prepares package review/approval and route startup
through their APIs, then uses production Compose controls for consent, thread
start, message text input, two full reads, approval, stop and release. Both direct
and forwarding-only ProxyJump routes use the same explicit no-model Python
fixture as macOS. The test checks all ten files and the exact complete Unicode
diff, readonly/selectable semantics, stale-version RPC refusal and the original
child approval response. It also checks the independent remote unknown outcome
when the Host closes the long-lived Session. It does not cover package import or
launch-form UI, a real Agent/model, or user hardware.

```sh
JAVA_TOOL_OPTIONS="-Djava.io.tmpdir=$PWD/tmp" ./android/gradlew -p android -PghostermPluginJs=true :app:assembleDebug :app:assembleDebugAndroidTest
python3 -B scripts/test-plugin-codex-patch-android.py --headless-api 36 --gterm tmp/codex-gterm/gterm --route both
```
