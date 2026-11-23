# Codex

Command `ai codex`.

The Executor uses the declared Codex CLI through ProcessService. The Lua implementation handles initialization, thread creation, text turns, and interruption. Approval and file-change flows are still being migrated.

Build from the repository root.

```sh
python3 scripts/gterm-plugin pack plugins/official/ai-codex --out tmp/ai-codex.zip
```

The static declaration is in `main.lua`. See the
[plugin bridge plan](../../../docs/reference/plan-plugin-bridge.md) for verification status.
