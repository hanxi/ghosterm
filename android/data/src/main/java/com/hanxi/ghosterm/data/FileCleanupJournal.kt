package com.hanxi.ghosterm.data

import android.content.ContentValues
import android.database.DatabaseErrorHandler
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteException
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.util.UUID

enum class FileCleanupState { NOT_ATTEMPTED, ABSENT, CHANGED, UNKNOWN, REJECTED, REMOVED }
data class FileCleanupRecord(val id:String,val originId:String,val relativePath:String,val candidate:Boolean,
    val endpoint:String,val parent:String,val entryId:String,val name:String,val directory:Boolean,
    val observedMillis:Long,val updatedMillis:Long,val state:FileCleanupState=FileCleanupState.UNKNOWN,
    val status:Int?=null,val remoteStatus:UInt?=null,val error:String?=null)

/** Independent write-ahead maintenance observations; never rewrites copy history.
 * An intent without a final acknowledgement remains UNKNOWN after process death.
 * It never grants permission to resume or replay deletion. */
class FileCleanupJournal(private val file:File) {
    companion object { const val MAX_RECORDS=64;private const val MAX_BYTES=4*1024*1024 }
    private val mutex=Mutex()
    private suspend fun <T> transaction(block:(SQLiteDatabase)->T):T=withContext(Dispatchers.IO) {
        mutex.withLock {
            val parent=checkNotNull(file.parentFile)
            check(parent.isDirectory || parent.mkdirs()) { "无法创建清理记录目录" }
            check(OsConstants.S_ISDIR(Os.lstat(parent.path).st_mode)) { "清理记录目录无效" }
            Os.chmod(parent.path,448)
            val existed=file.exists()
            for(path in listOf(file,File(file.path+"-journal"),File(file.path+"-wal"),File(file.path+"-shm"))) {
                val stat=try { Os.lstat(path.path) } catch(e:ErrnoException) { if(e.errno==OsConstants.ENOENT)null else throw e }
                check(stat==null || OsConstants.S_ISREG(stat.st_mode) && stat.st_size<=16L*1024*1024) { "清理记录文件无效或过大" }
            }
            SQLiteDatabase.openDatabase(file.path,null,SQLiteDatabase.OPEN_READWRITE or SQLiteDatabase.CREATE_IF_NECESSARY or
                SQLiteDatabase.NO_LOCALIZED_COLLATORS,DatabaseErrorHandler { throw SQLiteException("清理记录损坏，原文件已保留") }).use { db ->
                db.execSQL("PRAGMA synchronous=FULL")
                db.rawQuery("PRAGMA max_page_count=4096",null).use { check(it.moveToFirst() && it.getLong(0)<=4096) }
                db.beginTransaction()
                try {
                    if(!existed) { db.execSQL("CREATE TABLE cleanup (id TEXT PRIMARY KEY NOT NULL,payload TEXT NOT NULL)");db.version=1 }
                    check(db.version==1) { "清理记录版本不支持，原文件已保留" }
                    Os.chmod(file.path,384)
                    val result=block(db);db.setTransactionSuccessful();result
                } finally { db.endTransaction() }
            }
        }
    }
    private fun encode(r:FileCleanupRecord):String {
        require(UUID.fromString(r.id).toString()==r.id && UUID.fromString(r.originId).toString()==r.originId)
        require((r.status==0)==(r.state==FileCleanupState.REMOVED || r.state==FileCleanupState.ABSENT))
        require(r.relativePath.isNotEmpty() && r.name.isNotEmpty() && r.observedMillis>=0 && r.updatedMillis>=0)
        return JSONObject().put("schema",1).put("id",r.id).put("origin",r.originId).put("relative",r.relativePath)
            .put("candidate",r.candidate).put("endpoint",r.endpoint).put("parent",r.parent).put("entry",r.entryId)
            .put("name",r.name).put("directory",r.directory).put("observed",r.observedMillis).put("updated",r.updatedMillis)
            .put("state",r.state.name).put("status",r.status ?: JSONObject.NULL)
            .put("remote",r.remoteStatus?.toLong() ?: JSONObject.NULL).put("error",r.error ?: JSONObject.NULL)
            .toString().also { check(it.toByteArray().size<=65536) { "清理记录过大" } }
    }
    private fun decode(payload:String):FileCleanupRecord {
        check(payload.toByteArray().size<=65536)
        val j=JSONObject(payload);check(j.getInt("schema")==1)
        return FileCleanupRecord(j.getString("id"),j.getString("origin"),j.getString("relative"),j.getBoolean("candidate"),
            j.getString("endpoint"),j.getString("parent"),j.getString("entry"),j.getString("name"),j.getBoolean("directory"),
            j.getLong("observed"),j.getLong("updated"),FileCleanupState.valueOf(j.getString("state")),
            if(j.isNull("status"))null else j.getInt("status"),if(j.isNull("remote"))null else j.getLong("remote").also { check(it in 0..0xffffffffL) }.toUInt(),
            if(j.isNull("error"))null else j.getString("error")).also { encode(it) }
    }
    private fun rows(db:SQLiteDatabase):List<FileCleanupRecord> {
        db.rawQuery("PRAGMA quick_check(1)",null).use { check(it.moveToFirst() && it.getString(0)=="ok" && !it.moveToNext()) { "清理记录完整性检查失败" } }
        var bytes=0
        return buildList {
            db.rawQuery("SELECT id,payload FROM cleanup ORDER BY rowid DESC",null).use { cursor ->
                while(cursor.moveToNext()) {
                    check(size<MAX_RECORDS);val payload=cursor.getString(1);bytes+=payload.toByteArray().size;check(bytes<=MAX_BYTES)
                    add(decode(payload).also { check(it.id==cursor.getString(0)) })
                }
            }
        }
    }
    suspend fun load():List<FileCleanupRecord> = transaction(::rows)
    suspend fun begin(record:FileCleanupRecord)=transaction { db ->
        require(record.state==FileCleanupState.UNKNOWN && record.status==null && record.remoteStatus==null)
        val existing=rows(db);check(existing.size<MAX_RECORDS) { "清理记录已满，请先清除不再需要的记录" }
        val payload=encode(record);check(existing.sumOf { encode(it).toByteArray().size }+payload.toByteArray().size<=MAX_BYTES)
        db.insertOrThrow("cleanup",null,ContentValues().apply { put("id",record.id);put("payload",payload) });Unit
    }
    suspend fun finish(record:FileCleanupRecord)=transaction { db ->
        val previous=rows(db).single { it.id==record.id }
        check(previous.state==FileCleanupState.UNKNOWN && previous.status==null) { "清理结果不能被重复改写" }
        check(record.copy(state=previous.state,status=previous.status,remoteStatus=previous.remoteStatus,error=previous.error,
            updatedMillis=previous.updatedMillis)==previous) { "清理结果不属于原操作" }
        val payload=encode(record)
        check(db.update("cleanup",ContentValues().apply { put("payload",payload) },"id=?",arrayOf(record.id))==1)
    }
    /** Explicit record removal only; never touches the corresponding location. */
    suspend fun remove(id:String)=transaction { db -> rows(db);db.delete("cleanup","id=?",arrayOf(id));Unit }
}
