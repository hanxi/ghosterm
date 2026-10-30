package com.hanxi.ghosterm.domain

/** IDs are backend identifiers; SAF document IDs are never converted to filesystem paths. */
data class FileEndpoint(val id: String, val label: String, val rootId: String, val treeUri: String? = null) {
    companion object { val APP = FileEndpoint("app", "应用文件", "") }
}
data class FileLocation(val endpoint: FileEndpoint, val directoryId: String = endpoint.rootId)
enum class FileKind { DIRECTORY, FILE, LINK, OTHER }
data class BrowserEntry(val id: String, val name: String, val kind: FileKind,
    val size: Long?, val modifiedMillis: Long?)
interface FileBrowserSource {
    suspend fun endpoints(): List<FileEndpoint>
    suspend fun grantTree(uri: String, flags: Int): FileEndpoint
    suspend fun list(location: FileLocation): List<BrowserEntry>
    suspend fun preview(location: FileLocation, entry: BrowserEntry): String
}
