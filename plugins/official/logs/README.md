# Paged log search

The Portable Executor generates deterministic sample records for `logs --count 5000`.
It retains a logical window of up to 4096 records and generates sample text within
each requested page. The Client browses that window using the standard native table
on macOS and Android. A host with `logs.page` negotiated can also search the active
Session. Search input is currently available through RPC.

Start a read with `after: 0` and `through: 0`. Continue with the returned
`cursor` as `after` and the returned `latest` as `through`. Keep the same query
until the cursor reaches that upper bound. An empty page can still advance the
cursor. The Executor scans at most 128 records and returns at most four per call.
`truncated` reports when retention removed part of the requested range.
