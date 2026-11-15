# Native views demo

Two independent computed tables, one bounded read-only diff and one cancellable in-memory counter.
Client requires ui.views and all three standard components. Missing services are unsupported; no fallback.
Closing a Native view does not cancel its task; the explicit views.reopen RPC asks the live Client to reopen a known view ID.
The same Runtime keeps cancellation identity and observations across reopening.
No files, subprocesses, models, accounts, installation hooks or credential persistence.
