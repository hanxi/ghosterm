# Candidate Portable diff plugin

`diff --before '"alpha\nbeta\n"' --after '"alpha\ngamma\n"'` compares two
explicit JSON-encoded text values. JSON encoding keeps raw newlines out of the
launch command. Optional `--before-label` and `--after-label` provide display names.

The Executor computes a deterministic, deletion-first longest common subsequence.
It preserves Unicode code units, LF/CRLF/CR and missing final newline. Each input
has at most 128 lines of 128 Unicode code points and 3,072 encoded UTF-8 bytes.
Comparisons retain a bounded edit script; RPC returns at most three rows and
2,048 JSON UTF-8 bytes at a time. The transport's own argv/command limits still
apply, so a long local invocation may exceed the SSH launch command limit.
Preparation advances through the existing bounded Native worker hook; describe
and page requests await the complete comparison. Closing wakes pending queries
and revokes their access. The standard CPU and RPC budgets are unchanged.

`view.diff.describe` and `view.diff.page` bind view identity, revision and the
exact selected page. Line numbers and before/after cursors remain explicit.
Rows are read-only data; labels are display names, and no patch is applied.
The package requires only Session RPC on the Executor, with no file, process,
network or account service. The same Executor runs locally or through owned SSH.

The Client requires the standard Native `ui.diff` service. The macOS Host provides
a read-only Native view with exact line endings and previous/next paging. Android
rendering is pending and must reject the missing service explicitly. The candidate
package is built by the developer tool and is not installed or released by default.
