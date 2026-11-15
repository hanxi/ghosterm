# Candidate Portable table plugin

`table --count 1000` computes a read-only table of integers, squares and cubes.
It generates at most sixteen rows per RPC, including exact decimal cubes near
10^18, and never allocates the complete dataset. `--count 0` is an empty table.
No file, process, network or account capability is used.

The same checked Executor can run under a local `gterm` or authenticated SSH
exec. It registers `view.table.describe` and `view.table.page`; a Native Host
selects those methods through the existing Session RPC allowlist and consent.
Requests bind the view identity, revision, offset and limit. The Client requires
`ui.table`: the experimental JS-enabled macOS Host supplies a standard Native
table and page controls for commands declaring `views: ["table"]`. Android and
other Hosts without this service must report unsupported. The Client service is
an explicit Session requirement (`gterm --client-capability ui.table`), separate
from the Executor's own services and subject to the same Native consent.

The Portable SDK exports structural codecs and semantic validation for at most
eight columns, sixteen rows and 2,048 JSON UTF-8 bytes per table message. Generic
Native table controls retain one selected page, reject late responses after
Runtime replacement/closure, and show unknown/error separately from an empty
table. macOS uses Native text, scroll and buttons; Android UI and physical-device
evidence remain pending. This package is not installed or released by default.
