package com.hanxi.ghosterm.domain

import java.util.Collections
import java.util.UUID

/** Immutable selection metadata, not file contents or the Android text clipboard.
 * A future paste must revalidate permissions, endpoint identity and source attributes.
 */
class FileClipboardSnapshot private constructor(
    val id: String,
    val source: FileLocation,
    val sourcePathLabel: String,
    val items: List<BrowserEntry>,
    val createdAtMillis: Long
) {
    companion object {
        fun capture(source: FileLocation, sourcePathLabel: String, items: List<BrowserEntry>): FileClipboardSnapshot {
            require(items.isNotEmpty() && items.size <= 4096) { "请选择 1 至 4096 个项目" }
            require(items.map { it.id }.toSet().size == items.size) { "来源条目身份重复，请刷新后重试" }
            return FileClipboardSnapshot(UUID.randomUUID().toString(), source, sourcePathLabel,
                Collections.unmodifiableList(ArrayList(items)), System.currentTimeMillis())
        }
    }
}
