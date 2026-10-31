package com.hanxi.ghosterm.data

import android.content.ContentValues
import android.database.DatabaseErrorHandler
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteException
import android.system.Os
import android.system.OsConstants
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.util.Collections

/** Private, transactional observations. Corrupt/unknown data is preserved, never reset.
 * Each call owns and closes its connection on IO. SQLite serializes other cooperating
 * instances; the mutex also bounds the application's own pending database work.
 * No credentials, content snapshots, connection owners or conflict authority are stored.
 */
class FileTaskJournal(private val file: File) {
    companion object {
        const val MAX_RECORDS = 64
        const val MAX_ITEMS = 32768
        const val MAX_BYTES = 16L * 1024 * 1024
        private const val MAX_FILE_BYTES = 64L * 1024 * 1024
    }
    private val mutex=Mutex()
    private suspend fun <T> transaction(block: (SQLiteDatabase) -> T): T = withContext(Dispatchers.IO) {
        mutex.withLock {
            open().use { db ->
                db.beginTransaction()
                try { val result=block(db);db.setTransactionSuccessful();result }
                finally { db.endTransaction() }
            }
        }
    }
    private fun open(): SQLiteDatabase {
        val parent=checkNotNull(file.parentFile)
        check(parent.isDirectory || parent.mkdirs()) { "无法创建任务记录目录" }
        check(OsConstants.S_ISDIR(Os.lstat(parent.path).st_mode)) { "任务记录目录无效" }
        Os.chmod(parent.path,448)
        val existed=file.exists()
        // Include dangling links; File.exists alone follows links and would miss them.
        for(path in listOf(file,File(file.path+"-journal"),File(file.path+"-wal"),File(file.path+"-shm"))) {
            val stat=try { Os.lstat(path.path) } catch(e: android.system.ErrnoException) {
                if(e.errno==OsConstants.ENOENT) null else throw e
            }
            if(stat!=null) check(OsConstants.S_ISREG(stat.st_mode) && stat.st_size<=MAX_FILE_BYTES) { "任务记录文件无效或过大" }
        }
        val db=SQLiteDatabase.openDatabase(file.path,null,
            SQLiteDatabase.OPEN_READWRITE or SQLiteDatabase.CREATE_IF_NECESSARY or SQLiteDatabase.NO_LOCALIZED_COLLATORS,
            DatabaseErrorHandler { throw SQLiteException("任务记录损坏，已保留原文件") })
        try {
            db.execSQL("PRAGMA foreign_keys=ON")
            db.execSQL("PRAGMA synchronous=FULL")
            db.rawQuery("PRAGMA journal_mode=DELETE",null).use { check(it.moveToFirst() && it.getString(0)=="delete") }
            db.rawQuery("PRAGMA busy_timeout=1000",null).use { check(it.moveToFirst() && it.getInt(0)==1000) }
            db.rawQuery("PRAGMA max_page_count=16384",null).use { check(it.moveToFirst() && it.getLong(0)<=16384) }
            if(!existed) {
                db.beginTransaction()
                try {
                    db.execSQL("CREATE TABLE tasks (id TEXT PRIMARY KEY NOT NULL, phase TEXT NOT NULL, payload BLOB NOT NULL)")
                    db.execSQL("CREATE TABLE items (task TEXT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE, path TEXT NOT NULL, payload BLOB NOT NULL, PRIMARY KEY(task,path))")
                    db.execSQL("CREATE TABLE budget (id INTEGER PRIMARY KEY CHECK(id=1), bytes INTEGER NOT NULL, items INTEGER NOT NULL)")
                    db.execSQL("INSERT INTO budget VALUES (1,0,0)")
                    db.version=1;db.setTransactionSuccessful()
                } finally { db.endTransaction() }
            }
            check(db.version==1) { "任务记录版本不支持，原记录已保留" }
            Os.chmod(file.path,384)
            return db
        } catch(error: Throwable) { db.close();throw error }
    }
    private fun scalar(db: SQLiteDatabase, sql: String, args: Array<String>?=null): Long =
        db.rawQuery(sql,args).use { check(it.moveToFirst());val n=it.getLong(0);check(!it.moveToNext());n }
    private fun header(db: SQLiteDatabase,id: String): FileJournalRecord =
        db.rawQuery("SELECT payload FROM tasks WHERE id=?",arrayOf(id)).use {
            check(it.moveToFirst()) { "任务记录不存在" }
            FileJournalCodec.readHeader(it.getBlob(0)).also { record -> check(record.intent.id==id) }
        }
    private fun budget(db: SQLiteDatabase, byteDelta: Long, itemDelta: Int=0) {
        val bytes=scalar(db,"SELECT bytes FROM budget WHERE id=1")+byteDelta
        val items=scalar(db,"SELECT items FROM budget WHERE id=1")+itemDelta
        check(bytes in 0..MAX_BYTES && items in 0..MAX_ITEMS.toLong()) { "任务记录容量已满，请清除不再需要的历史记录" }
        db.execSQL("UPDATE budget SET bytes=?,items=? WHERE id=1",arrayOf(bytes,items))
    }
    private fun writeHeader(db: SQLiteDatabase, record: FileJournalRecord) {
        val payload=FileJournalCodec.header(record)
        val old=scalar(db,"SELECT length(payload) FROM tasks WHERE id=?",arrayOf(record.intent.id))
        budget(db,payload.size-old)
        check(db.update("tasks",ContentValues().apply { put("payload",payload);put("phase",record.phase.name) },"id=?",arrayOf(record.intent.id))==1)
    }
    suspend fun load(): List<FileJournalRecord> = transaction { db ->
        db.rawQuery("PRAGMA quick_check(1)",null).use { check(it.moveToFirst() && it.getString(0)=="ok" && !it.moveToNext()) { "任务记录完整性检查失败，原记录已保留" } }
        check(scalar(db,"SELECT count(*) FROM tasks")<=MAX_RECORDS)
        check(scalar(db,"SELECT count(*) FROM items")<=MAX_ITEMS)
        var bytes=0L;var count=0
        val records=ArrayList<FileJournalRecord>()
        db.rawQuery("SELECT id,payload,phase FROM tasks",null).use { tasks ->
            while(tasks.moveToNext()) {
                val payload=tasks.getBlob(1);bytes+=payload.size
                val record=FileJournalCodec.readHeader(payload);check(record.intent.id==tasks.getString(0) && record.phase.name==tasks.getString(2))
                val items=ArrayList<FileJournalItem>()
                db.rawQuery("SELECT path,payload FROM items WHERE task=? ORDER BY rowid",arrayOf(record.intent.id)).use { rows ->
                    while(rows.moveToNext()) {
                        check(items.size<16384 && ++count<=MAX_ITEMS)
                        val body=rows.getBlob(1);bytes+=body.size;check(bytes<=MAX_BYTES)
                        val item=FileJournalCodec.readItem(body);check(item.value.source.relativePath==rows.getString(0))
                        items.add(item)
                    }
                }
                records.add(record.copy(items=Collections.unmodifiableList(items)))
            }
        }
        check(bytes<=MAX_BYTES && bytes==scalar(db,"SELECT bytes FROM budget WHERE id=1") &&
            count.toLong()==scalar(db,"SELECT items FROM budget WHERE id=1") &&
            count.toLong()==scalar(db,"SELECT count(*) FROM items")) { "任务记录账目不一致，原记录已保留" }
        Collections.unmodifiableList(records.sortedByDescending { it.updatedMillis })
    }
    suspend fun accept(request: FilePasteRequest) = transaction { db ->
        check(scalar(db,"SELECT count(*) FROM tasks")<MAX_RECORDS) { "任务历史已满，请先清除旧记录" }
        val record=FileJournalRecord(FileJournalIntent(request.id,request.clipboardId,request.source,request.sourcePathLabel,
            request.items,request.destination,request.destinationPathLabel),FileJournalPhase.QUEUED,System.currentTimeMillis())
        val payload=FileJournalCodec.header(record);budget(db,payload.size.toLong())
        db.insertOrThrow("tasks",null,ContentValues().apply { put("id",request.id);put("phase",record.phase.name);put("payload",payload) })
        Unit
    }
    suspend fun phase(id: String, phase: FileJournalPhase) = transaction { db ->
        require(phase!=FileJournalPhase.FINISHED && phase!=FileJournalPhase.RUNNING)
        val record=header(db,id);check(record.phase!=FileJournalPhase.FINISHED)
        writeHeader(db,record.copy(phase=phase,updatedMillis=System.currentTimeMillis()))
    }
    /** Whole plan commits before the first destination side effect; no partial plan on quota failure. */
    suspend fun begin(id: String, items: List<FileCopyItemResult>) = transaction { db ->
        require(items.size<=16384 && items.distinctBy { it.source.relativePath }.size==items.size)
        val record=header(db,id);check(record.phase==FileJournalPhase.CONFIRMING)
        check(scalar(db,"SELECT count(*) FROM items WHERE task=?",arrayOf(id))==0L)
        for(item in items) {
            val payload=FileJournalCodec.item(FileJournalItem(item,FileCopyCheckpoint.PREPARING))
            budget(db,payload.size.toLong(),1)
            db.insertOrThrow("items",null,ContentValues().apply { put("task",id);put("path",item.source.relativePath);put("payload",payload) })
        }
        writeHeader(db,record.copy(phase=FileJournalPhase.RUNNING,updatedMillis=System.currentTimeMillis()))
    }
    suspend fun checkpoint(id: String,value: FileCopyItemResult,point: FileCopyCheckpoint) = transaction { db ->
        db.rawQuery("SELECT phase FROM tasks WHERE id=?",arrayOf(id)).use {
            check(it.moveToFirst() && it.getString(0) in setOf("RUNNING","CANCELLING"))
        }
        val old=db.rawQuery("SELECT payload FROM items WHERE task=? AND path=?",arrayOf(id,value.source.relativePath)).use {
            check(it.moveToFirst()) { "任务条目未登记" };it.getBlob(0)
        }
        check(FileJournalCodec.readItem(old).value.source==value.source) { "任务条目身份已改变" }
        val payload=FileJournalCodec.item(FileJournalItem(value,point));budget(db,(payload.size-old.size).toLong())
        check(db.update("items",ContentValues().apply { put("payload",payload) },"task=? AND path=?",arrayOf(id,value.source.relativePath))==1)
    }
    suspend fun finish(id: String, result: FileCopyProgress?, cancelled: Boolean, error: String?) = transaction { db ->
        val record=header(db,id)
        require(result==null || result.requestId==id && result.clipboardId==record.intent.clipboardId)
        // Reconcile retained actual results atomically with the terminal header. A prior
        // checkpoint error must not leave COMPLETE in memory but RUNNING on disk.
        result?.items?.forEach { value ->
            val old=db.rawQuery("SELECT payload FROM items WHERE task=? AND path=?",arrayOf(id,value.source.relativePath)).use {
                check(it.moveToFirst()) { "任务条目未登记" };it.getBlob(0)
            }
            check(FileJournalCodec.readItem(old).value.source==value.source)
            val payload=FileJournalCodec.item(FileJournalItem(value,FileCopyCheckpoint.RESULT))
            budget(db,(payload.size-old.size).toLong())
            check(db.update("items",ContentValues().apply { put("payload",payload) },"task=? AND path=?",arrayOf(id,value.source.relativePath))==1)
        }
        writeHeader(db,record.copy(phase=FileJournalPhase.FINISHED,updatedMillis=System.currentTimeMillis(),
            outcome=result?.state,cancelled=cancelled,error=error ?: result?.error))
    }
    /** Clearing history never touches destination or candidate files. */
    suspend fun remove(id: String) = transaction { db ->
        val body=scalar(db,"SELECT length(payload) FROM tasks WHERE id=?",arrayOf(id))
        val items=scalar(db,"SELECT count(*) FROM items WHERE task=?",arrayOf(id))
        val bytes=scalar(db,"SELECT coalesce(sum(length(payload)),0) FROM items WHERE task=?",arrayOf(id))
        budget(db,-body-bytes,-items.toInt())
        check(db.delete("tasks","id=?",arrayOf(id))==1)
    }
}
