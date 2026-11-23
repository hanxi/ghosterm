# Claude

Command `ai claude`.

The Executor uses the declared Claude CLI through ProcessService. The Lua implementation supports single-turn text streaming. Tool approval and interactive follow-up flows are still being migrated.

Build from the repository root.

```sh
python3 scripts/gterm-plugin pack plugins/official/ai-claude --out tmp/ai-claude.zip
```

The static declaration is in `main.lua`. See the
[plugin bridge plan](../../../docs/reference/plan-plugin-bridge.md) for verification status.
