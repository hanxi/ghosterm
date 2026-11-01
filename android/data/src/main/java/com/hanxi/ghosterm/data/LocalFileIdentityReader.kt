package com.hanxi.ghosterm.data

import android.os.ParcelFileDescriptor
import android.os.Process
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructStat
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import java.io.File
import java.util.UUID

/** Read-only creation observations. No content check, result promotion or cleanup authority.
 * An xattr alone is insufficient: the inode, UID and selected parent must also match.
 * Same-UID hostile writers and backup/restore are outside this observation contract. */
class LocalFileIdentityReader(private val home:File,private val tasks:File,
    private val checkpoint:()->Unit = {}) {
    private data class Stamp(val device:Long,val inode:Long,val uid:Int,val mode:Int,val links:Long,
        val size:Long,val modifiedSeconds:Long,val modifiedNanos:Long,val changedSeconds:Long,val changedNanos:Long)
    private fun stamp(s:StructStat)=Stamp(s.st_dev,s.st_ino,s.st_uid,s.st_mode,s.st_nlink,s.st_size,
        s.st_mtim.tv_sec,s.st_mtim.tv_nsec,s.st_ctim.tv_sec,s.st_ctim.tv_nsec)
    private fun named(parent:ParcelFileDescriptor,name:String):StructStat? = try {
        Os.lstat("/proc/self/fd/${parent.fd}/$name")
    } catch(error:ErrnoException) { if(error.errno==OsConstants.ENOENT)null else throw error }
    private fun marker(file:ParcelFileDescriptor):ByteArray? = try {
        Os.getxattr("/proc/self/fd/${file.fd}",LocalCreationMarker.NAME)
    } catch(error:ErrnoException) { if(error.errno==OsConstants.ENODATA)null else throw error }
    private fun same(a:ByteArray?,b:ByteArray?)=if(a==null)b==null else b!=null && a.contentEquals(b)
    private fun directory(fd:ParcelFileDescriptor,privateDirectory:Boolean) {
        val s=Os.fstat(fd.fileDescriptor)
        check(OsConstants.S_ISDIR(s.st_mode) && s.st_uid==Process.myUid() && (!privateDirectory || (s.st_mode and 511)==448)) { "核对目录的类型、归属或权限已改变" }
    }
    suspend fun inspect(record:FileJournalRecord,item:FileJournalItem,candidate:Boolean=false):LocalFileIdentityResult = withContext(Dispatchers.IO) {
        require(item in record.items && record.items.count { it.value.source.relativePath==item.value.source.relativePath }==1)
        val key=FileHistoryContentKey(record.intent.id,item.value.source.relativePath,candidate)
        fun result(state:LocalFileIdentityState,error:String?=null)=LocalFileIdentityResult(key,state,error=error)
        val reference=item.value.localIdentity ?: return@withContext result(LocalFileIdentityState.NO_REFERENCE,"这条记录没有本机文件创建身份参考")
        try {
            ensureActive()
            require(reference.uid==Process.myUid()) { "创建记录不属于当前应用身份" }
            val parent=checkNotNull(item.value.targetParent)
            require(parent.endpoint==FileEndpoint.APP && record.intent.destination.endpoint==FileEndpoint.APP &&
                item.value.source.entry.kind==FileKind.FILE) { "这条记录不是应用本机文件" }
            val position=if(candidate) {
                val path=item.value.candidatePath
                val plan=if(path!=null) {
                    val prefix=tasks.absolutePath+File.separator
                    require(path.startsWith(prefix)) { "暂存位置不在本应用的候选区域" }
                    val parts=path.removePrefix(prefix).split('/')
                    require(parts.size==3 && UUID.fromString(parts[0]).toString()==parts[0] && parts[1]=="destination" &&
                        parts[2]==reference.candidateName && File(tasks,parts.joinToString("/")).absolutePath==path) { "暂存位置结构或文件名不一致" }
                    require(record.executionPlanId==null || record.executionPlanId==parts[0]) { "暂存位置与执行计划不一致" }
                    parts[0]
                } else checkNotNull(record.executionPlanId) { "没有可核对的暂存计划位置" }
                require(UUID.fromString(plan).toString()==plan)
                plan to reference.candidateName
            } else {
                val name=checkNotNull(item.value.targetName)
                require(name.isNotEmpty() && name!="." && name!=".." && '/' !in name && '\u0000' !in name)
                val id=parent.directoryId.trimEnd('/').let { if(it.isEmpty())name else "$it/$name" }
                require(item.value.createdId==null || item.value.createdId==id) { "目标位置记录不一致" }
                null to name
            }
            val plan=position.first;val name=position.second
            fun openParent():ParcelFileDescriptor? {
                if(!candidate)return LocalPublicationFile.directory(home,parent.directoryId).also { fd ->
                    try { directory(fd,false) } catch(error:Throwable) { fd.close();throw error }
                }
                val rootStat=try { Os.lstat(tasks.path) } catch(error:ErrnoException) { if(error.errno==OsConstants.ENOENT)return null else throw error }
                check(OsConstants.S_ISDIR(rootStat.st_mode)) { "暂存根目录不是普通目录" }
                LocalPublicationFile.directory(tasks,"").use { root ->
                    directory(root,true)
                    if(named(root,checkNotNull(plan))==null)return null
                    LocalPublicationFile.childDirectory(root.fd,plan).use { owner ->
                        directory(owner,true)
                        if(named(owner,"destination")==null)return null
                        return LocalPublicationFile.childDirectory(owner.fd,"destination").also { fd ->
                            try { directory(fd,true) } catch(error:Throwable) { fd.close();throw error }
                        }
                    }
                }
            }
            val selected=openParent() ?: return@withContext result(LocalFileIdentityState.MISSING)
            selected.use { selectedParent ->
                val expectedDevice=if(candidate)reference.candidateDevice else reference.destinationDevice
                val expectedInode=if(candidate)reference.candidateInode else reference.destinationInode
                fun matchingParent(fd:ParcelFileDescriptor):Boolean {
                    val s=Os.fstat(fd.fileDescriptor);return s.st_dev==expectedDevice && s.st_ino==expectedInode
                }
                if(!matchingParent(selectedParent))return@withContext result(LocalFileIdentityState.DIFFERENT,"当前位置的目录与创建时不同")
                val initial=named(selectedParent,name) ?: return@withContext result(LocalFileIdentityState.MISSING)
                if(!OsConstants.S_ISREG(initial.st_mode) || initial.st_nlink!=1L || initial.st_uid!=reference.uid ||
                    initial.st_dev!=reference.device || initial.st_ino!=reference.inode)
                    return@withContext result(LocalFileIdentityState.DIFFERENT)
                LocalPublicationFile.retainedCandidate(selectedParent.fd,name).use { file ->
                    val before=stamp(Os.fstat(file.fileDescriptor))
                    check(before==stamp(initial)) { "核对期间条目发生变化" }
                    checkpoint();ensureActive()
                    val token=marker(file)
                    try {
                        check(stamp(Os.fstat(file.fileDescriptor))==before) { "核对期间文件信息发生变化" }
                        val fresh=openParent() ?: error("核对期间目录已消失")
                        fresh.use { freshParent ->
                            check(matchingParent(freshParent)) { "核对期间目录位置发生变化" }
                            LocalPublicationFile.retainedCandidate(freshParent.fd,name).use { current ->
                                check(stamp(Os.fstat(current.fileDescriptor))==before) { "核对期间文件位置发生变化" }
                                val currentToken=marker(current)
                                try { check(same(token,currentToken)) { "核对期间创建标记发生变化" } }
                                finally { currentToken?.fill(0) }
                            }
                        }
                        ensureActive()
                        return@withContext result(if(token!=null && token.contentEquals(reference.token.toByteArray(Charsets.US_ASCII)))
                            LocalFileIdentityState.SAME else LocalFileIdentityState.DIFFERENT)
                    } finally { token?.fill(0) }
                }
            }
        } catch(cancel:CancellationException) { throw cancel }
        catch(error:Exception) { result(LocalFileIdentityState.UNKNOWN,error.message ?: "无法核对本机文件身份") }
    }
}
