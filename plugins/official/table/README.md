# Candidate Portable table plugin

`table --count 1000` computes a read-only table of integers, squares and cubes.
It generates at most sixteen rows per RPC, including exact decimal cubes near
10^18, and never allocates the complete dataset. `--count 0` is an empty table.
No file, process, network or account capability is used.

The same checked Executor can run under a local `gterm` or authenticated SSH
exec. It registers `view.table.describe` and `view.table.page`; a Native Host
selects those methods through the existing Session RPC allowlist and consent.
Requests bind the view identity, revision, offset and limit. The Client requires
`ui.table`: current native Apps do not yet advertise that service, so importing
this candidate must report unsupported instead of pretending a table is shown.

The Portable SDK exports structural codecs and semantic validation for at most
eight columns, sixteen rows and 2,048 JSON UTF-8 bytes per table message. Generic
Native table components, pagination UI, accessibility, theme and device evidence
are the next P5 slice; this package is not an installed or released App feature.
