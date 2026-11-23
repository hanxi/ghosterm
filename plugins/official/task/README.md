# Task

Command `task primes`.

Runs a sample prime-counting task with progress and cancellation through the native task view.

Build from the repository root.

```sh
python3 scripts/gterm-plugin pack plugins/official/task --out tmp/task.zip
```

The static declaration is in `main.lua`. See the
[plugin bridge plan](../../../docs/reference/plan-plugin-bridge.md) for verification status.
