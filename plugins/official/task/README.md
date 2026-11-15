# Candidate Portable task plugin

`task primes --limit 1000000` runs an actual bounded sieve counting primes from
2 through the supplied limit (0..1,000,000). It uses the existing Native worker
hook, with at most 512 primitive work units per pump. The retained sieve has at
most 1,000,001 bytes, is allocated on the first work slice, and is released when
complete or disposed. Progress counts
candidates classified, not elapsed work, an ETA or a percentage of CPU time.

The same Executor has no file, process, network or account dependency and can
run locally or over an explicitly selected SSH transport. It requires Session
RPC only. The Client requires the standard Native `ui.task` service; Native
renderers are pending and missing services must fail explicitly. This candidate
is not installed, enabled or released by default.

`view.task.describe` returns the current view identity and revision.
`view.task.read` returns a bounded observation with a monotonic sequence,
exact uint64 progress, known/unknown total and a separate confirmed result.
`view.task.cancel` accepts one request identity for the instance and returns
`requested` or `alreadyFinished`. A repeated identical request returns the same
acknowledgement without repeating work; a different cancellation identity after
the first one is rejected. Acknowledgement does not mean the job has stopped.
The worker observes cancellation before its next work slice and then publishes
a confirmed `cancelled` snapshot. A late cancel preserves a completed result.

Unknown RPC results do not prove rejection or cancellation. A Host retains the
request identity, does not automatically replay it, and may explicitly read the
same active task to observe its state. Disposal or loss of the instance revokes
reads and is not a fabricated cancelled/success result. A known terminal result
remains a historical observation; unknown current work must remain unknown.
