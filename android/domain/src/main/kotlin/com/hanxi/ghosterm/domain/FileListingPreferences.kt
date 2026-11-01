package com.hanxi.ghosterm.domain

import java.util.Locale

/** A presentation-only view. Original metadata and frozen clipboard items stay intact. */
fun AppPreferences.orderFiles(entries: List<BrowserEntry>): List<BrowserEntry> =
    entries.filter { showHidden || !it.name.startsWith(".") }.sortedWith(Comparator { a, b ->
        var order = if (foldersFirst) (a.kind != FileKind.DIRECTORY).compareTo(b.kind != FileKind.DIRECTORY) else 0
        if (order == 0 && sortBy == FileSort.SIZE) {
            // Directory metadata is not a measured file byte count. Unknown sizes sort last.
            val first = a.size.takeIf { a.kind == FileKind.FILE }
            val second = b.size.takeIf { b.kind == FileKind.FILE }
            order = when {
                first == null && second == null -> 0
                first == null -> 1
                second == null -> -1
                else -> second.compareTo(first)
            }
        }
        if (order == 0) order = a.name.lowercase(Locale.ROOT).compareTo(b.name.lowercase(Locale.ROOT))
        if (order == 0) order = a.name.compareTo(b.name)
        if (order == 0) order = a.id.compareTo(b.id)
        order
    })
