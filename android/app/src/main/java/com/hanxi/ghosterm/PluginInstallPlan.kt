package com.hanxi.ghosterm

data class PluginInstallPlan(
    val source: PluginGitHubSource,
    val releaseTag: String,
    val publisher: String,
    val publisherFingerprint: String,
    val items: List<Item>,
    val overwriteIds: List<String>,
    val pinRelease: Boolean
) {
    data class Item(val id: String, val version: String, val role: Role, val archiveBytes: Long, val archiveSha256: String, val permissions: List<String>, val capabilities: List<String>, val target: String?, val fixed: Boolean)
    enum class Role { CLIENT, EXECUTOR, BOTH }
    val isValid get() = items.isNotEmpty() && items.map { it.id }.toSet().size == items.size && items.all { it.archiveBytes > 0 && it.archiveSha256.length == 64 && (it.role != Role.EXECUTOR || it.target != null) } && (!pinRelease || releaseTag.isNotEmpty())
}
