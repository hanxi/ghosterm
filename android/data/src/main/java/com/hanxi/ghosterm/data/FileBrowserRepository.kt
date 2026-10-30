package com.hanxi.ghosterm.data

import com.hanxi.ghosterm.bridge.RemoteFileSession
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.Closeable
import java.util.Locale
import java.util.UUID

/** Process-local file connections, independent of terminal owners and transfer jobs.
 * Map access is serialized by the caller's UI dispatcher; native requests run off it. */
class FileBrowserRepository(private val local: FileBrowserSource) : FileBrowserSource, Closeable {
    private val remotes = linkedMapOf<String, Pair<FileEndpoint,RemoteFileSession>>()
    fun attach(identity: RemoteFileIdentity, connection: RemoteFileSession): FileEndpoint {
        val endpoint = FileEndpoint("remote:${UUID.randomUUID()}",identity.alias,".",remote=identity)
        remotes[endpoint.id] = endpoint to connection
        return endpoint
    }
    fun disconnect(endpoint: FileEndpoint) { remotes.remove(endpoint.id)?.second?.close() }
    override fun close() { remotes.values.forEach { it.second.close() };remotes.clear() }
    override suspend fun endpoints(): List<FileEndpoint> = local.endpoints() + remotes.values.map { it.first }
    override suspend fun grantTree(uri: String, flags: Int) = local.grantTree(uri,flags)
    private fun session(endpoint: FileEndpoint): RemoteFileSession {
        val pair=remotes[endpoint.id]
        check(pair!=null && pair.first==endpoint) { "文件连接已断开，请重新连接此主机" }
        check(pair.second.state().ready) { "文件连接已断开，请重新连接此主机" }
        return pair.second
    }
    override suspend fun list(location: FileLocation) = browse(location).entries
    override suspend fun browse(location: FileLocation): BrowserDirectory {
        if(location.endpoint.remoteAlias==null) return local.browse(location)
        val directory=session(location.endpoint).browse(location.directoryId)
        return withContext(Dispatchers.Default) {
            val entries=directory.entries.map {
                BrowserEntry(it.path,it.name,when {
                    it.symbolicLink -> FileKind.LINK
                    it.directory -> FileKind.DIRECTORY
                    it.regularFile -> FileKind.FILE
                    else -> FileKind.OTHER
                },it.size,it.modifiedSeconds?.takeIf { seconds -> seconds<=Long.MAX_VALUE/1000 }?.times(1000))
            }.sortedWith(compareBy<BrowserEntry> { it.kind!=FileKind.DIRECTORY }
                .thenBy { it.name.lowercase(Locale.ROOT) }.thenBy { it.name }.thenBy { it.id })
            BrowserDirectory(location.copy(directoryId=directory.path),entries)
        }
    }
    override suspend fun preview(location: FileLocation, entry: BrowserEntry): String {
        if(location.endpoint.remoteAlias==null) return local.preview(location,entry)
        check(entry.kind==FileKind.FILE) { "此条目不能预览；符号链接不会自动跟随" }
        val prefix=location.directoryId.trimEnd('/')+"/"
        check(entry.id.startsWith(prefix) && entry.id.removePrefix(prefix).let { it.isNotEmpty() && '/' !in it && it!="." && it!=".." }) { "文件不属于当前文件夹" }
        check(entry.size?.let { it<=LocalFileBrowser.PREVIEW_LIMIT.toULong() } != false) { "只读预览最多支持 256 KiB 文本" }
        return session(location.endpoint).previewText(entry.id)
    }
}
