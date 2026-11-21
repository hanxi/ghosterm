# Paged log search

The Portable Executor generates deterministic sample records for `logs --count 5000`.
It keeps at most 4096 records in memory and accepts read-only `logs.page` requests
on the active Session. The Client currently exposes no native log view.

Start a read with `after: 0` and `through: 0`. Continue with the returned
`cursor` as `after` and the returned `latest` as `through`. Keep the same query
until the cursor reaches that upper bound. An empty page can still advance the
cursor. The Executor scans at most 128 records and returns at most four per call.
`truncated` reports when retention removed part of the requested range.
