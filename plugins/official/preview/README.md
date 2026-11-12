# Ghosterm Preview

The first official resource preview plugin. The Executor publishes one user-selected relative path from the explicitly configured `workspace` root. HTML and Markdown may list up to six explicitly selected CSS or raster image files with `--assets path1,path2`; the plugin does not scan directories or discover files implicitly. Every path remains scoped to the same selected workspace root.

The host command is `preview <path> [--max-bytes N] [--assets path1,path2]`. Paths must be canonical relative paths of at most 512 characters. The default per-file budget is 256 KiB; the per-file ceiling is 1 MiB. The Client receives a verified immutable snapshot for the main file and each listed asset. The current macOS resource pane renders bounded UTF-8, Markdown, static raster images, and restricted single-document HTML. Asset snapshots are transferred, while relative HTML loading is being wired into the native preview.

Select the plugin folder in the Extensions pane, review and approve its exact package digest, set the Executor package directory to this folder, and provide an explicit remote `workspace` directory before launch. The portable runtime must be enabled in the host and `gterm` must be installed on the target.
