# Ghosterm Preview

The first official resource preview plugin. The Executor publishes one user-selected relative path from the explicitly configured `workspace` root. It never chooses the current directory, home directory, package directory, or another root automatically.

The host command is `preview <path> [--max-bytes N]`. The default transfer budget is 256 KiB; the per-invocation ceiling is 1 MiB. The Client receives a verified immutable snapshot. The current macOS resource pane renders bounded UTF-8, Markdown, static raster images, and restricted single-document HTML. External HTML dependencies are not loaded.

Select the plugin folder in the Extensions pane, review and approve its exact package digest, set the Executor package directory to this folder, and provide an explicit remote `workspace` directory before launch. The portable runtime must be enabled in the host and `gterm` must be installed on the target.
