#!/usr/bin/env bash
# Build the Lua-only CLI and exercise the Core and signed foreground paths.
set -euo pipefail

root="$(git rev-parse --show-toplevel)"
mkdir -p "$root/tmp"
run="$(mktemp -d "$root/tmp/agent-XXXXXX")"
trap 'status=$?; rm -rf -- "$run"; test ! -e "$run" || exit 2; exit "$status"' EXIT
trap 'exit 130' HUP INT TERM
export TMPDIR="$run" TMP="$run" TEMP="$run" PYTHONDONTWRITEBYTECODE=1
cd "$root"

archive="${GHOSTERM_LUA_ARCHIVE:-$run/lua-5.5.1.tar.gz}"
if [[ -z "${GHOSTERM_LUA_ARCHIVE:-}" ]]; then
    curl --fail --silent --show-error --location --retry 3 \
        https://www.lua.org/ftp/lua-5.5.1.tar.gz -o "$archive"
fi
build="$run/build"
cmake -S cli/gterm -B "$build" -DCMAKE_BUILD_TYPE=Debug \
    -DGHOSTERM_BUILD_TESTS=ON -DGHOSTERM_GTERM_TESTS=ON -DGHOSTERM_WITH_PLUGIN_LUA=ON \
    -DGHOSTERM_WITH_RELEASE_SIGNATURE=ON -DGHOSTERM_WITH_LIBSSH2=ON \
    "-DGHOSTERM_LUA_ARCHIVE=$archive"
cmake --build "$build" --parallel 4 --target gterm test_lua_preview test_lua_client test_lua_budget \
    test_ext_resource test_process_prefix test_package_check test_process_channel test_process_channel_child \
    test_ext_package test_plugin_registry test_ext_runtime_ownership test_runtime_views test_gterm_live test_gterm_multiplex
nm -g "$build/gterm" "$build/core/libghosterm_core.a" > "$run/plugin-symbols.txt"
grep -Eq '[[:space:]][Tt][[:space:]]_?lua_newstate$' "$run/plugin-symbols.txt"
if grep -Eq '[[:space:]]_?JS_(NewRuntime|NewContext|Eval|RunGC)$' "$run/plugin-symbols.txt"; then
    echo 'The Lua build still contains QuickJS symbols' >&2
    exit 1
fi
ctest --test-dir "$build/core" --no-tests=error \
    -R '^(lua_(preview_resource_roundtrip|vm_budget)$|lua_(package|runtime|views)_|process_channel_)' --output-on-failure
if [[ "$(uname -s)" == Darwin ]]; then
    GHOSTERM_CORE_BUILD_DIR="$run/swift-core" GHOSTERM_BUILD_PLUGIN_LUA=1 \
        GHOSTERM_LUA_ARCHIVE="$archive" bash scripts/build-core.sh
    GHOSTERM_CORE_BUILD_DIR="$run/swift-core" GHOSTERM_CORE_TEST_PLUGIN_LUA=1 \
        swift test --package-path apple/GhostermCore --scratch-path "$run/swift" \
        --filter 'ExtensionRuntime(Availability)?Tests'
fi
python3 -B scripts/test-plugin-package-native.py --binary "$build/core/test_package_check" --oom-sweep
python3 -B scripts/test-gterm-multiplex.py --build "$build"
python3 -B scripts/gterm-plugin build plugins/official/preview --out "$run/preview-package"
"$build/core/test_lua_preview" --package "$run/preview-package" "$run"
python3 -B scripts/gterm-plugin build plugins/official/ai-claude --out "$run/claude-package"
for mode in roundtrip count pointer null-item argument-bytes total-bytes combined-count combined-bytes \
    count-boundary revoked memory; do
    "$build/core/test_process_prefix" "$mode" "$run" "$run/claude-package"
done
mkdir "$run/legacy"
printf '{}\n' > "$run/legacy/plugin.json"
printf '%s\n' '{"side":"client","sdkVersion":"1.0.0","protocols":[1],"runtimes":["lua"],"capabilities":["session.rpc","resource.receive"]}' > "$run/host.json"
if "$build/gterm" plugin check --host-json "$run/host.json" -- "$run/legacy" > "$run/legacy-result.json"; then
    echo 'Legacy package was accepted' >&2
    exit 1
else
    [[ $? -eq 4 ]]
    grep -q 'LEGACY_PACKAGE_REINSTALL' "$run/legacy-result.json"
fi
"$build/gterm" plugin check --host-json "$run/host.json" -- "$run/preview-package" > "$run/check-result.json"
for retired in old.JS old.ts old.luac package.json package-lock.json; do
    mixed="$run/mixed-$retired"
    python3 -B - "$run/preview-package" "$mixed" "$retired" <<'PY'
import hashlib
import json
from pathlib import Path
import shutil
import sys

source, mixed = map(Path, sys.argv[1:3])
name = sys.argv[3]
shutil.copytree(source, mixed)
payload = b"{}" if name.endswith(".json") else b"fixture"
(mixed / name).write_bytes(payload)
index = mixed / "integrity.json"
manifest = json.loads(index.read_bytes())
manifest["members"].append(dict(path=name, bytes=len(payload),
                                sha256=hashlib.sha256(payload).hexdigest()))
index.write_text(json.dumps(manifest))
PY
    if "$build/gterm" plugin check --host-json "$run/host.json" -- "$mixed" > "$run/mixed-result.json"; then
        echo "Lua package with retired member $retired was accepted" >&2
        exit 1
    else
        [[ $? -eq 5 ]]
        grep -q 'INTEGRITY' "$run/mixed-result.json"
    fi
    printf 'PASS Lua package rejects %s\n' "$retired"
done
python3 -B scripts/test-lua-preview-ssh.py "$build/core/test_lua_preview" --gterm "$build/gterm"
python3 -B scripts/test-gterm-ssh.py --host-binary "$build/core/test_ext_resource" --gterm "$build/gterm"
python3 -B scripts/test-gterm-ssh.py --host-binary "$build/core/test_ext_resource" --gterm "$build/gterm" --resource
python3 -B scripts/test-gterm-lua-release.py "$build/gterm" "$build/core/test_lua_preview" \
    --export-official-fixtures "$run/public-fixtures"
test "$(find "$run/public-fixtures" -name release.sig -type f | wc -l | tr -d ' ')" -eq 9
python3 -B scripts/gterm-plugin init "$run/client" --id org.ghosterm.devclient --command hello --publisher Fixture
python3 -B - "$run/client/main.lua" "$run" <<'PY'
from pathlib import Path
import sys

source, work = map(Path, sys.argv[1:])
text = source.read_text()
(work / "static-only.lua").write_text(text.replace("return {", 'error("manifest extraction executed Lua")\nreturn {', 1))
(work / "dynamic.lua").write_text(text.replace('publisher = "Fixture"', 'publisher = tostring("Fixture")', 1))
(work / "duplicate.lua").write_text(text.replace('publisher = "Fixture"', 'publisher = "Fixture", publisher = "Other"', 1))
command = 'title = "hello"'
assert text.count(command) == 1
(work / "toggle-without-view.lua").write_text(text.replace(command, command + ', togglePanel = true'))
(work / "toggle-wrong-type.lua").write_text(text.replace(command, command + ', views = { "table" }, togglePanel = "true"'))
PY
python3 -B scripts/gterm-plugin pack "$run/static-only.lua" --out "$run/static-first.zip"
python3 -B scripts/gterm-plugin pack "$run/static-only.lua" --out "$run/static-second.zip"
cmp "$run/static-first.zip" "$run/static-second.zip"
for invalid in dynamic duplicate; do
    if python3 -B scripts/gterm-plugin lua-manifest "$run/$invalid.lua" >"$run/$invalid-manifest.log" 2>&1; then
        echo "Accepted an invalid Lua manifest: $invalid" >&2
        exit 1
    else
        [[ $? -eq 2 ]]
    fi
done
grep -Fq 'expected a static table, string, integer or boolean' "$run/dynamic-manifest.log"
grep -Fq 'duplicate manifest key' "$run/duplicate-manifest.log"
for invalid in toggle-without-view toggle-wrong-type; do
    if python3 -B scripts/gterm-plugin check "$run/$invalid.lua" >"$run/$invalid.log" 2>&1; then
        echo "Accepted an invalid panel toggle: $invalid" >&2
        exit 1
    else
        [[ $? -eq 2 ]]
    fi
    grep -Fq 'togglePanel requires a boolean and a declared view when enabled' "$run/$invalid.log"
done
python3 -B scripts/gterm-plugin build plugins/official/table --out "$run/toggle-package"
printf '%s\n' '{"side":"client","sdkVersion":"1.0.0","protocols":[1],"runtimes":["lua"],"capabilities":["session.rpc","ui.table"]}' > "$run/toggle-host.json"
"$build/gterm" plugin check --host-json "$run/toggle-host.json" -- "$run/toggle-package" > "$run/toggle-checked.json"
python3 -B -c 'import json, sys; assert json.load(open(sys.argv[1]))["commands"][0]["togglePanel"] is True' "$run/toggle-checked.json"
python3 -B scripts/gterm-plugin test "$run/client/main.lua" --core-build "$build/core" --command hello

awk '/^```lua$/{example=1; next} /^```$/{if (example) exit} example' \
    docs/reference/plan-plugin-bridge.md > "$run/documented-hello.lua"
"$build/gterm" plugin test "$run/documented-hello.lua" --core-build "$build/core" --command hello
"$build/gterm" plugin check "$run/documented-hello.lua"
"$build/gterm" plugin pack "$run/documented-hello.lua" --out "$run/hello-build.zip"
cmake --install "$build" --prefix "$run/installed-gterm"
"$run/installed-gterm/bin/gterm" plugin pack "$run/documented-hello.lua" --out "$run/hello-install.zip"
cmp "$run/hello-build.zip" "$run/hello-install.zip"
"$run/installed-gterm/bin/gterm" plugin release --help > "$run/dev-release-help.txt"
python3 -B - "$run/client/main.lua" "$run/sandbox.lua" <<'PY'
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
output.write_text(source.read_text().replace("function()", "function(ctx)").replace("    return true", """    local total = 0
    for _, n in ipairs({ 2, 3 }) do total = total + n end
    for _, n in pairs({ value = 4 }) do total = total + n end
    assert(tonumber(tostring(total)) == 9)
    assert(os == nil and io == nil and debug == nil and package == nil)
    assert(load == nil and loadfile == nil and dofile == nil and require == nil and _G == nil)
    local payload = "x"
    for _ = 1, 18 do payload = payload .. payload end
    payload = payload .. utf8.char(0x1f600)
    assert(ctx.json.encode({ data = payload }) == nil)
    local json = assert(ctx.json.encode({ data = payload }, 524288))
    assert(ctx.json.decode(json) == nil)
    assert(ctx.json.decode(json, #json - 1) == nil)
    assert(ctx.json.encode({ data = payload }, 1048577) == nil)
    assert(ctx.json.decode(json, 1048577) == nil)
    assert(ctx.json.decode(json, #json).data == payload)
    assert(ctx.json.decode('{"same":1,"same":2}', 65536) == nil)
    assert(ctx.json.decode('{"same":1,"same":2,"data":"' .. payload .. '"}', 524288) == nil)
    local array = assert(ctx.json.decode('["' .. payload .. '",null,-1.5,true]', 524288))
    assert(ctx.json.isArray(array) and array[1] == payload and array[2] == ctx.json.null)
    assert(array[3] == -1.5 and array[4] == true and not ctx.json.isArray({}))
    return true"""))
PY
python3 -B scripts/gterm-plugin test "$run/sandbox.lua" --core-build "$build/core" --command hello
sed 's/return true/return nil/' "$run/client/main.lua" > "$run/failing-client.lua"
if python3 -B scripts/gterm-plugin test "$run/failing-client.lua" --core-build "$build/core" --command hello; then
    echo 'Lua command failure was reported as success' >&2
    exit 1
else
    [[ $? -eq 5 ]]
fi
mkdir "$run/pair"
cat > "$run/pair/main.lua" <<'LUA'
local manifest = {
  id = "org.ghosterm.devrpc", version = "0.1.0", api = "1", publisher = "Fixture", license = "MIT",
  client = { requires = { "session.rpc" } },
  executor = { entry = "executor.lua", requires = { "session.rpc" }, rpc = { "fixture.echo" } },
  commands = { { path = { "echo" }, title = "Echo", argsSchema = "args.json" } },
}
return {
  echo = function(ctx)
    assert(ctx.context.workspaceId == "workspace-a" and ctx.context.paneId == "pane-b")
    assert(ctx.context.directory == "/owned/project")
    local expected = "x"
    for _ = 1, 12 do expected = expected .. expected end
    assert(ctx.arguments.payload == expected:sub(1, 3500))
    local cycle = {}
    cycle.self = cycle
    local deep = {}
    for _ = 1, 34 do deep = { child = deep } end
    for _, invalid in ipairs({
      cycle,
      deep,
      { [2] = "sparse" },
      { [1] = true, mixed = true },
      { too_large = 9007199254740992 },
    }) do
      local rejected, err = ctx.rpc.call("fixture.echo", invalid, 1000)
      assert(rejected == nil and err.code == "RPC_INVALID_INPUT")
    end
    local reply = ctx.rpc.call("fixture.echo", { value = 41 }, 1000)
    if reply and reply.value == 42 then return true end
    return nil
  end,
}
LUA
cat > "$run/pair/args.json" <<'JSON'
{"type":"object","properties":{"payload":{"type":"string","maxLength":4096}},"required":["payload"],"additionalProperties":false,"x-ghosterm-cli":{"version":1,"fields":{"payload":{"position":0}}}}
JSON
cat > "$run/pair/executor.lua" <<'LUA'
local ctx = ...
assert(ctx.context == nil)
local expected = "x"
for _ = 1, 12 do expected = expected .. expected end
assert(ctx.arguments.payload == expected:sub(1, 3500))
return { ["fixture.echo"] = function(input) return { value = input.value + 1 } end }
LUA
payload="$(python3 -B -c 'print("x" * 3500)')"
GHOSTERM_TEST_LUA_CONTEXT=1 python3 -B scripts/gterm-plugin test "$run/pair" --paired \
    --core-build "$build/core" --command echo --arg "$payload"

# The static declaration is valid in both cases. Activation must reject a
# missing handler and a handler added outside the declared RPC surface.
for variant in missing extra; do
    candidate="$run/pair-$variant"
    cp -R "$run/pair" "$candidate"
    if [[ "$variant" == missing ]]; then
        printf 'return {}\n' > "$candidate/executor.lua"
    else
        printf 'return { ["fixture.echo"] = function() end, ["fixture.extra"] = function() end }\n' \
            > "$candidate/executor.lua"
    fi
    python3 -B scripts/gterm-plugin check "$candidate"
    if GHOSTERM_TEST_LUA_CONTEXT=1 python3 -B scripts/gterm-plugin test "$candidate" --paired --core-build "$build/core" \
        --command echo --arg "$payload" > "$candidate.log" 2>&1; then
        echo "Lua Executor accepted $variant RPC export" >&2
        exit 1
    else
        [[ $? -eq 5 ]]
    fi
    grep -q 'TEST_CLIENT_ACTIVATION' "$candidate.log"
    grep -Eq 'Lua Executor activation failed: phase=[0-9]+ error=6$' "$candidate.log"
done
