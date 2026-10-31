package com.hanxi.ghosterm.data

import android.content.res.AssetFileDescriptor
import android.os.ParcelFileDescriptor
import android.content.ContentResolver
import android.content.Intent
import android.database.Cursor
import android.net.Uri
import android.os.CancellationSignal
import android.provider.DocumentsContract as Documents
import android.provider.DocumentsContract.Document as Document
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import java.io.File
import java.io.FileDescriptor
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.nio.file.Files
import java.util.Locale
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/** Read-only browsing. Root is the terminal HOME, never the app's identity containers. */
class LocalFileBrowser(private val root: File, private val resolver: ContentResolver) : FileBrowserSource {
    companion object {
        const val PREVIEW_LIMIT = 256 * 1024
        const val ENTRY_LIMIT = 4096
        private val columns = arrayOf(Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
            Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED)
    }
    private suspend fun <T> io(block: (CancellationSignal, () -> Boolean) -> T): T = coroutineScope { suspendCancellableCoroutine { waiting ->
        val signal = CancellationSignal()
        waiting.invokeOnCancellation { Dispatchers.IO.dispatch(EmptyCoroutineContext) { signal.cancel() } }
        launch(Dispatchers.IO) {
            try { if (waiting.isActive) { val result = block(signal) { waiting.isActive }; if (waiting.isActive) waiting.resume(result) } }
            catch (failure: Exception) { if (waiting.isActive) waiting.resumeWithException(failure) }
        }
    } }
    private fun tree(value: String): Uri = Uri.parse(value).also {
        require(it.scheme == "content" && Documents.isTreeUri(it)) { "请选择系统授权的文件夹" }
    }
    private fun checkGrant(endpoint: FileEndpoint): Uri {
        val uri = tree(checkNotNull(endpoint.treeUri))
        check(resolver.persistedUriPermissions.any { it.uri == uri && it.isReadPermission }) { "文件夹授权已失效，请重新选择设备文件夹" }
        return uri
    }
    private fun document(uri: Uri, id: String) = Documents.buildDocumentUriUsingTree(uri, id)
    private fun Cursor.entry(): BrowserEntry {
        fun text(column: String) = getString(getColumnIndexOrThrow(column))
        fun number(column: String): Long? { val i=getColumnIndexOrThrow(column);return if(isNull(i)) null else getLong(i).takeIf { it>=0 } }
        val id=checkNotNull(text(Document.COLUMN_DOCUMENT_ID)) { "文档缺少标识" }
        val name=checkNotNull(text(Document.COLUMN_DISPLAY_NAME)) { "文档缺少名称" }
        check(id.isNotEmpty() && name.isNotEmpty()) { "文档标识或名称为空" }
        val kind=if(text(Document.COLUMN_MIME_TYPE)==Document.MIME_TYPE_DIR) FileKind.DIRECTORY else FileKind.FILE
        return BrowserEntry(id,name,kind,if(kind==FileKind.FILE) number(Document.COLUMN_SIZE)?.toULong() else null,number(Document.COLUMN_LAST_MODIFIED))
    }
    private fun info(uri: Uri, signal: CancellationSignal): BrowserEntry =
        checkNotNull(resolver.query(uri,columns,null,null,null,signal)) { "无法读取文档信息" }.use {
            check(it.moveToFirst()) { "文档已移动或不存在" }; it.entry()
        }
    override suspend fun endpoints(): List<FileEndpoint> = io { signal, active ->
        listOf(FileEndpoint.APP) + resolver.persistedUriPermissions.filter { it.isReadPermission && Documents.isTreeUri(it.uri) }.map {
            signal.throwIfCanceled(); check(active())
            val id=Documents.getTreeDocumentId(it.uri)
            val label=try { info(document(it.uri,id),signal).name } catch (_: Exception) { "设备文件夹（不可访问）" }
            FileEndpoint(it.uri.toString(),label,id,it.uri.toString())
        }.sortedBy { it.label }
    }
    override suspend fun grantTree(uri: String, flags: Int): FileEndpoint = io { signal, _ ->
        val selected=tree(uri); val granted=flags and (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        check(granted and Intent.FLAG_GRANT_READ_URI_PERMISSION != 0) { "没有取得文件夹读取权限" }
        val id=Documents.getTreeDocumentId(selected);val entry=info(document(selected,id),signal)
        check(entry.kind==FileKind.DIRECTORY) { "请选择文件夹" }
        resolver.takePersistableUriPermission(selected,granted)
        FileEndpoint(uri,entry.name,id,uri)
    }
    private fun appPath(id: String): File {
        check(root.isDirectory || root.mkdirs()) { "无法准备应用文件夹" }
        check(OsConstants.S_ISDIR(Os.lstat(root.path).st_mode)) { "应用文件夹不能是符号链接" }
        if(id.isEmpty()) return root
        val parts=id.split('/')
        require(parts.none { it.isEmpty() || it=="." || it==".." || '\u0000' in it }) { "无效的文件路径" }
        var path=root
        parts.forEachIndexed { index, part ->
            path=File(path,part)
            if(index<parts.lastIndex) check(OsConstants.S_ISDIR(Os.lstat(path.path).st_mode)) { "不能穿过符号链接或非文件夹" }
        }
        return path
    }
    private fun appEntry(id: String, file: File): BrowserEntry {
        val stat=Os.lstat(file.path)
        val kind=when { OsConstants.S_ISDIR(stat.st_mode)->FileKind.DIRECTORY;OsConstants.S_ISREG(stat.st_mode)->FileKind.FILE
            OsConstants.S_ISLNK(stat.st_mode)->FileKind.LINK;else->FileKind.OTHER }
        return BrowserEntry(id,file.name,kind,if(kind==FileKind.FILE) stat.st_size.toULong() else null,stat.st_mtime*1000)
    }
    override suspend fun list(location: FileLocation): List<BrowserEntry> = io { signal, active ->
        val entries=ArrayList<BrowserEntry>()
        fun add(entry: BrowserEntry) {
            signal.throwIfCanceled();check(active());check(entries.size<ENTRY_LIMIT) { "文件夹超过 4096 项，无法完整列出" };entries.add(entry)
        }
        if(location.endpoint==FileEndpoint.APP) {
            val directory=appPath(location.directoryId)
            check(appEntry(location.directoryId,directory).kind==FileKind.DIRECTORY) { "该位置不是文件夹或是符号链接" }
            Files.newDirectoryStream(directory.toPath()).use { stream -> stream.forEach {
                val name=it.fileName.toString();val id=if(location.directoryId.isEmpty()) name else location.directoryId+"/"+name
                add(appEntry(id,it.toFile()))
            } }
        } else {
            val tree=checkGrant(location.endpoint)
            check(info(document(tree,location.directoryId),signal).kind==FileKind.DIRECTORY) { "该文档不是文件夹" }
            val uri=Documents.buildChildDocumentsUriUsingTree(tree,location.directoryId)
            checkNotNull(resolver.query(uri,columns,null,null,null,signal)) { "文件夹读取失败" }.use { cursor ->
                while(cursor.moveToNext()) add(cursor.entry())
            }
            check(entries.map { it.id }.distinct().size==entries.size) { "文件提供方返回重复文档标识" }
        }
        entries.sortedWith(compareBy<BrowserEntry> { it.kind!=FileKind.DIRECTORY }
            .thenBy { it.name.lowercase(Locale.ROOT) }.thenBy { it.name }.thenBy { it.id })
    }
    private fun read(fd: FileDescriptor, length: Long, active: () -> Boolean): String {
        check(length<=PREVIEW_LIMIT) { "只读预览最多支持 256 KiB 文本" }
        val data=ByteArray(PREVIEW_LIMIT+1)
        val flags=Os.fcntlInt(fd,OsConstants.F_GETFL,0)
        Os.fcntlInt(fd,OsConstants.F_SETFL,flags or OsConstants.O_NONBLOCK)
        try {
            var used=0;val cap=if(length>=0) minOf(length, data.size.toLong()).toInt() else data.size
            val poll=StructPollfd().apply { this.fd=fd;events=OsConstants.POLLIN.toShort() }
            while(used<cap) {
                check(active()) { "预览已取消" }
                try {
                    if(Os.poll(arrayOf(poll),100)==0) continue
                    val count=Os.read(fd,data,used,cap-used);if(count==0) break;used+=count
                } catch(failure: ErrnoException) {
                    if(failure.errno!=OsConstants.EINTR && failure.errno!=OsConstants.EAGAIN) throw failure
                }
            }
            check(used<=PREVIEW_LIMIT) { "只读预览最多支持 256 KiB 文本" }
            check((0 until used).none { data[it]==0.toByte() }) { "此文件不是可预览的 UTF-8 文本" }
            return try { Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(data,0,used)).toString() }
            catch (_: java.nio.charset.CharacterCodingException) { error("此文件不是可预览的 UTF-8 文本") }
        } finally { data.fill(0);Os.fcntlInt(fd,OsConstants.F_SETFL,flags) }
    }
    /** Freeze a selected local/SAF source into an anonymous task-owned snapshot.
     * No destination is opened and no remote/browser connection is borrowed. */
    suspend fun snapshot(location: FileLocation, entry: BrowserEntry, store: TransferStagingStore,
        progress: (Long, Long?) -> Unit = { _, _ -> }): TransferStagingStore.Snapshot {
        check(entry.kind==FileKind.FILE) { "只能复制普通文件；符号链接不会自动跟随" }
        val expected=entry.size?.let { check(it<=Long.MAX_VALUE.toULong()) { "文件大小不受支持" };it.toLong() }
        return store.snapshot(expected,progress) { signal ->
            if(location.endpoint==FileEndpoint.APP) {
                check(entry.id.substringBeforeLast('/',"")==location.directoryId) { "文件不属于当前文件夹" }
                val file=appPath(entry.id)
                val fd=Os.open(file.path,OsConstants.O_RDONLY or OsConstants.O_CLOEXEC or OsConstants.O_NONBLOCK or OsConstants.O_NOFOLLOW,0)
                try {
                    check(OsConstants.S_ISREG(Os.fstat(fd).st_mode)) { "只能复制普通文件" }
                    AssetFileDescriptor(ParcelFileDescriptor.dup(fd),0,AssetFileDescriptor.UNKNOWN_LENGTH)
                } finally { Os.close(fd) }
            } else {
                check(location.endpoint.remote==null) { "远端来源须使用独立任务连接" }
                val tree=checkGrant(location.endpoint);val uri=document(tree,entry.id)
                check(Documents.isChildDocument(resolver,document(tree,location.directoryId),uri)) { "文件不属于来源文件夹" }
                check(info(uri,signal).kind==FileKind.FILE) { "该文档不是文件" }
                checkNotNull(resolver.openAssetFileDescriptor(uri,"r",signal)) { "无法打开文档" }
            }
        }
    }
    override suspend fun preview(location: FileLocation, entry: BrowserEntry): String = io { signal, active ->
        check(entry.kind==FileKind.FILE) { "此条目不能预览；符号链接不会自动跟随" }
        if(location.endpoint==FileEndpoint.APP) {
            val parent=entry.id.substringBeforeLast('/',"")
            check(parent==location.directoryId) { "文件不属于当前文件夹" }
            val file=appPath(entry.id)
            val fd=Os.open(file.path,OsConstants.O_RDONLY or OsConstants.O_CLOEXEC or OsConstants.O_NONBLOCK or OsConstants.O_NOFOLLOW,0)
            try {
                val current=Os.fstat(fd);check(OsConstants.S_ISREG(current.st_mode)) { "只能预览普通文件" }
                // Size is a preflight check only: a growing file is still bounded by the read limit.
                check(current.st_size<=PREVIEW_LIMIT) { "只读预览最多支持 256 KiB 文本" }
                read(fd,-1,active)
            } finally {
                // Android FileInputStream(FileDescriptor) borrows the descriptor.
                // This owner opened it with Os.open, so it must also close it.
                Os.close(fd)
            }
        } else {
            val uri=document(checkGrant(location.endpoint),entry.id)
            val current=info(uri,signal);check(current.kind==FileKind.FILE) { "该文档不是文件" }
            val size=current.size
            check(size==null || size<=PREVIEW_LIMIT.toULong()) { "只读预览最多支持 256 KiB 文本" }
            checkNotNull(resolver.openAssetFileDescriptor(uri,"r",signal)) { "无法打开文档" }.use { asset ->
                asset.createInputStream().use { read(it.fd,asset.declaredLength,active) }
            }
        }
    }
}
