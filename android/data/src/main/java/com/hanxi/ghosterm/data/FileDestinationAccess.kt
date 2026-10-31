package com.hanxi.ghosterm.data

import android.content.ContentResolver
import android.net.Uri
import android.os.CancellationSignal
import android.provider.DocumentsContract as Documents
import android.provider.DocumentsContract.Document
import android.system.Os
import android.system.OsConstants
import android.system.ErrnoException
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import java.io.File
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/** Inspects existing destinations only. Remote browse uses the caller's dedicated owner.
 * Namespace checks do not prove that different providers/servers never alias storage. */
class FileDestinationAccess(private val home: File, private val resolver: ContentResolver,
    private val browse: suspend (FileLocation) -> BrowserDirectory) {
    private suspend fun <T> io(block: (CancellationSignal) -> T): T = coroutineScope { suspendCancellableCoroutine { waiting ->
        val signal=CancellationSignal()
        waiting.invokeOnCancellation { Dispatchers.IO.dispatch(EmptyCoroutineContext) { signal.cancel() } }
        launch(Dispatchers.IO) {
            try { if(waiting.isActive) { val result=block(signal);if(waiting.isActive) waiting.resume(result) } }
            catch(error: Exception) { if(waiting.isActive) waiting.resumeWithException(error) }
        }
    } }
    private fun tree(location: FileLocation, write: Boolean): Uri {
        val endpoint=location.endpoint
        require(endpoint.remote==null)
        val uri=Uri.parse(checkNotNull(endpoint.treeUri))
        check(uri.scheme=="content" && !uri.authority.isNullOrEmpty() && Documents.isTreeUri(uri)) { "无效的文档树" }
        check(Documents.getTreeDocumentId(uri)==endpoint.rootId) { "文档树身份已改变" }
        check(resolver.persistedUriPermissions.any { it.uri==uri && it.isReadPermission && (!write || it.isWritePermission) }) { "文件夹授权已失效或只读" }
        val root=Documents.buildDocumentUriUsingTree(uri,endpoint.rootId)
        val document=Documents.buildDocumentUriUsingTree(uri,location.directoryId)
        check(root==document || Documents.isChildDocument(resolver,root,document)) { "目录不在授权文档树内" }
        return uri
    }
    private fun canonicalPath(path: String, absolute: Boolean) {
        require(if(absolute) path.startsWith('/') else !path.startsWith('/')) { "目录路径未规范化" }
        val body=if(absolute)path.removePrefix("/") else path
        require('\u0000' !in path && (body.isEmpty() || body.split('/').none { it.isEmpty() || it=="." || it==".." })) { "无效的目录路径" }
    }
    suspend fun inspect(location: FileLocation): FileTargetDirectory {
        val create=when {
            location.endpoint==FileEndpoint.APP -> io {
                LocalPublicationFile.directory(home,location.directoryId).use { directory ->
                    val writable=try { Os.access("/proc/self/fd/${directory.fd}",OsConstants.W_OK or OsConstants.X_OK) }
                    catch(error: ErrnoException) {
                        if(error.errno==OsConstants.EACCES || error.errno==OsConstants.EROFS) false else throw error
                    }
                    if(writable) FileCreateCapability.ALLOWED else FileCreateCapability.DENIED
                }
            }
            location.endpoint.remote!=null -> {
                canonicalPath(location.directoryId,true)
                FileCreateCapability.UNKNOWN // SFTP listing/permission bits cannot establish effective write permission.
            }
            else -> io { signal ->
                val tree=tree(location,false)
                if(!resolver.persistedUriPermissions.any { it.uri==tree && it.isWritePermission }) FileCreateCapability.DENIED
                else {
                    val uri=Documents.buildDocumentUriUsingTree(tree,location.directoryId)
                    val columns=arrayOf(Document.COLUMN_DOCUMENT_ID,Document.COLUMN_MIME_TYPE,Document.COLUMN_FLAGS)
                    checkNotNull(resolver.query(uri,columns,null,null,null,signal)) { "无法读取目标目录信息" }.use { cursor ->
                        check(cursor.moveToFirst()) { "目标目录已不存在" }
                        check(cursor.getString(0)==location.directoryId && cursor.getString(1)==Document.MIME_TYPE_DIR) { "目标身份或类型已改变" }
                        check(!cursor.isNull(2)) { "无法确认目标创建能力" }
                        val flags=cursor.getInt(2)
                        check(!cursor.moveToNext()) { "目标目录身份不唯一" }
                        if(flags and Document.FLAG_DIR_SUPPORTS_CREATE!=0) FileCreateCapability.ALLOWED else FileCreateCapability.DENIED
                    }
                }
            }
        }
        currentCoroutineContext().ensureActive()
        // Denial does not require a second listing and must not be hidden by a read failure.
        val directory=if(create==FileCreateCapability.DENIED) BrowserDirectory(location,emptyList()) else browse(location)
        return FileTargetDirectory(directory,create)
    }
    suspend fun relation(source: FileLocation, target: FileLocation): FileLocationRelation {
        val a=source.endpoint;val b=target.endpoint
        if(a==FileEndpoint.APP && b==FileEndpoint.APP) {
            return io {
                LocalPublicationFile.directory(home,source.directoryId).close()
                LocalPublicationFile.directory(home,target.directoryId).close()
                pathRelation(source.directoryId,target.directoryId)
            }
        }
        if(a.treeUri!=null && b.treeUri!=null) return io {
            val sourceTree=tree(source,false);val targetTree=tree(target,false)
            if(sourceTree.authority!=targetTree.authority) FileLocationRelation.SEPARATE
            else {
                val parent=Documents.buildDocumentUriUsingTree(sourceTree,source.directoryId)
                val child=Documents.buildDocumentUriUsingTree(targetTree,target.directoryId)
                when {
                    source.directoryId==target.directoryId -> FileLocationRelation.SAME
                    Documents.isChildDocument(resolver,parent,child) -> FileLocationRelation.DESCENDANT
                    else -> FileLocationRelation.SEPARATE
                }
            }
        }
        if(a.remote!=null && b.remote!=null) {
            canonicalPath(source.directoryId,true);canonicalPath(target.directoryId,true)
            val left=checkNotNull(a.remote);val right=checkNotNull(b.remote)
            // Different connection UUIDs/aliases may still address the same namespace.
            // Different host/user/port cannot prove disjoint storage; leave explicit unknown.
            if(!left.host.equals(right.host,ignoreCase=true) || left.port!=right.port || left.user!=right.user)
                return FileLocationRelation.UNKNOWN
            return pathRelation(source.directoryId,target.directoryId)
        }
        check(a==FileEndpoint.APP || a.treeUri!=null || a.remote!=null) { "来源端点类型无效" }
        check(b==FileEndpoint.APP || b.treeUri!=null || b.remote!=null) { "目标端点类型无效" }
        return FileLocationRelation.SEPARATE
    }
    private fun pathRelation(source: String, target: String): FileLocationRelation = when {
        source==target -> FileLocationRelation.SAME
        target.startsWith(source.trimEnd('/')+"/") || source.isEmpty() -> FileLocationRelation.DESCENDANT
        else -> FileLocationRelation.SEPARATE
    }
}
