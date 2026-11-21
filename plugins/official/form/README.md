# Native form sample

Run the `form` command on a Host with the `ui.form` service. The Executor requests
three fields, waits up to two minutes, and keeps only the outcome kind. Both
platforms present the form in the active plugin pane. An authorized caller can
include the optional Executor method `form.status` in its Session allowlist to
read the current outcome, or `form.cancel` to request cancellation before an
answer arrives. Read the final outcome via `form.status`. The sample does not
persist the supplied field values.
