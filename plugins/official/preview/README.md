# Preview

Command `preview`.

Publishes a workspace file and explicitly listed companion assets through the resource channel. The Executor must have scoped file access to the selected workspace.

Build from the repository root.

```sh
python3 scripts/gterm-plugin pack plugins/official/preview --out tmp/preview.zip
```

The static declaration is in `main.lua`. See the
[plugin bridge plan](../../../docs/reference/plan-plugin-bridge.md) for verification status.
