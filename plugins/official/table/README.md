# Table

Command `table`.

Provides a paginated sample table through the standard native table view.
`table --count=16` selects the number of numeric rows. Use
`table --project=sample-a` or `table --project=sample-b` to display one of
two read-only sample project task lists.

Build from the repository root.

```sh
python3 scripts/gterm-plugin pack plugins/official/table --out tmp/table.zip
```

The static declaration is in `main.lua`. See the
[plugin bridge plan](../../../docs/reference/plan-plugin-bridge.md) for verification status.
