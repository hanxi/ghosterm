package com.hanxi.ghosterm

import android.os.SystemClock
import android.os.ParcelFileDescriptor
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.lang.reflect.InvocationTargetException
import java.net.Socket
import java.util.UUID
import kotlin.math.max

@RunWith(AndroidJUnit4::class)
class RemoteRemovalTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val fixture by lazy { sshFixture() }
    private val host = "files-fixture.ghosterm.test"
    private val peer get() = fixture.getJSONObject("channels")
    private fun native(name: String, vararg args: Any): Any? {
        val type = Class.forName("com.hanxi.ghosterm.bridge.NativeFiles")
        val method = type.declaredMethods.single { it.name == name }
        return try { method.invoke(type.getField("INSTANCE").get(null), *args) }
        catch(failure: InvocationTargetException) { throw failure.targetException }
    }
    private fun owners() = native("ownerCount") as Int
    private fun await(timeout: Long = 10000, message: String = "文件连接等待超时", test: () -> Boolean) {
        val end = SystemClock.uptimeMillis() + timeout
        do { if(test()) return; SystemClock.sleep(10) } while(SystemClock.uptimeMillis() < end)
        fail(message)
    }
    private fun limit(): Long {
        val start = SystemClock.uptimeMillis(); repeat(20) { SystemClock.sleep(10) }
        return max(2000L,(SystemClock.uptimeMillis()-start)*3/2+500)
    }
    private fun directory(block: (File) -> Unit) {
        val root = File(instrumentation.targetContext.cacheDir,"files-owner-${UUID.randomUUID()}").apply { mkdirs() }
        try { block(root) } finally { await(message="file owners must release") { owners()==0 };root.deleteRecursively() }
    }
    private fun known(root: File, protocol: Boolean = false) = File(root,if(protocol) "peer-known" else "known").apply {
        writeText("[$host]:${if(protocol) peer.getInt("port") else fixture.getInt("port")} " +
            (if(protocol) peer.getString("public") else fixture.getString("public")) + "\n")
    }
    private fun connection(file: File, protocol: Boolean = false, user: String = fixture.getString("user")) =
        ResolvedSSHConnection(host,if(protocol) peer.getInt("port") else fixture.getInt("port"),user,
            file.absolutePath,byteArrayOf(127,0,0,1),timeoutMillis=10000)
    private fun phase(files: RemoteFileSession, desired: SSHAuthPhase): SSHAuthState {
        var state = files.authentication()
        await {
            state = files.authentication()
            if(desired != SSHAuthPhase.FAILED) assertNotEquals(state.description,SSHAuthPhase.FAILED,state.phase)
            state.phase == desired
        }
        return state
    }
    private fun supply(files: RemoteFileSession, protocol: Boolean = false, ready: Boolean = true) {
        val key = fixture.getString(if(protocol) "pem" else "filePem").toByteArray()
        files.privateKey(phase(files,SSHAuthPhase.CREDENTIALS),key)
        assertTrue("file credential bytes erased",key.all { it == 0.toByte() })
        if(ready) await {
            val state=files.state();assertFalse("file connect status ${state.error}",state.finished);state.ready
        }
    }
    private fun control(op: String,user: String="",stage: String="realpath",action: String="allow"): JSONObject =
        Socket("127.0.0.1",peer.getInt("controlPort")).use { socket ->
            socket.soTimeout=5000
            socket.getOutputStream().write((JSONObject().put("token",peer.getString("token")).put("op",op)
                .put("user",user).put("stage",stage).put("action",action).toString()+"\n").toByteArray())
            JSONObject(socket.getInputStream().bufferedReader().readLine())
        }
    private fun reached(user: String, stage: String): Boolean {
        val events=control("state",user).getJSONArray("events")
        return (0 until events.length()).any { events.getString(it)==stage }
    }
    @Before fun priorOwnersReleased() { await { owners()==0 } }

    private val stages=listOf("remove-parent","remove-parent-real","remove-target","remove-parent-recheck","remove-target-recheck","remove-delete")
    private fun protocol(root:File,stage:String,action:String,block:suspend(RemoteFileSession,String)->Unit) {
        val user=control("new",stage=stage,action=action).getString("user")
        val files=RemoteFileSession(connection(known(root,true),true,user))
        try { supply(files,true);runBlocking { block(files,user) } }
        finally { control("release",user);runBlocking { files.closeAndAwait() };await { owners()==0 } }
    }
    private fun real(block:suspend(RemoteFileSession,String,File)->Unit)=directory { root ->
        val files=RemoteFileSession(connection(known(root)))
        try { supply(files);runBlocking { block(files,fixture.getString("remoteScratch"),root) } }
        finally { runBlocking { files.closeAndAwait() } }
    }
    private suspend fun create(files:RemoteFileSession,parent:String,root:File):RemoteFileEntry {
        val name="删除 ☃-${UUID.randomUUID()}"
        val source=File(root,"source-${UUID.randomUUID()}").apply { writeText("safe bytes ☃") }
        ParcelFileDescriptor.open(source,ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
            assertEquals(RemoteCreationState.COMPLETE,files.createExclusive("$parent/$name",fd.fd,source.length()).state)
        }
        val entry=files.browse(parent).entries.single { it.name==name }
        assertTrue(entry.regularFile);assertNotNull(entry.size);assertNotNull(entry.modifiedSeconds)
        return entry
    }
    @Test fun realFileRemovalPreservesAcknowledgementAndRejectsReplay()=real { files,parent,root ->
        val entry=create(files,parent,root)
        val request=files.prepareRemoval(parent,entry.name,false,entry.size!!.toLong(),entry.modifiedSeconds!!)
        assertNull(request.outcome.status);assertEquals(RemoteRemovalState.NOT_ATTEMPTED,request.outcome.state)
        val result=request.execute();request.awaitClosed()
        assertEquals(RemoteRemovalState.REMOVED,result.state);assertEquals(0,result.status);assertEquals(0u,result.remoteStatus)
        assertTrue(request.finished);assertTrue(files.browse(parent).entries.none { it.path==entry.path })
        val replay=runCatching { request.execute() }.exceptionOrNull()
        assertTrue("replay must fail at the one-shot admission guard",replay is IllegalStateException && replay !is CancellationException)
        assertEquals("删除请求不能重复执行",replay?.message)
        request.cancel();assertEquals(result,request.outcome)
    }
    @Test fun absenceIsAnObservationAndUnknownMetadataNeverStartsDeletion()=real { files,parent,_ ->
        assertThrows(IllegalArgumentException::class.java) { files.prepareRemoval(parent,"child",false,0,-1) }
        for(name in listOf("..","nested/child","bad\u0000name"))
            assertThrows(IllegalArgumentException::class.java) { files.prepareRemoval(parent,name,false,0,0) }
        assertThrows(IllegalArgumentException::class.java) { files.prepareRemoval("relative","child",false,0,0) }
        assertThrows(IllegalArgumentException::class.java) { files.prepareRemoval(parent,"x".repeat(256),false,0,0) }
        val request=files.prepareRemoval(parent,"absent-${UUID.randomUUID()}",false,0,0)
        val result=request.execute();request.awaitClosed()
        assertEquals(RemoteRemovalState.ABSENT,result.state);assertEquals(0,result.status);assertEquals(2u,result.remoteStatus)
        assertTrue(files.state().ready)
    }
    @Test fun changedMetadataAndNonemptyDirectoriesArePreserved() {
        for(change in listOf("size","time","nonempty")) real { files,parent,root ->
            val folder=if(change=="nonempty") files.prepareDirectory(parent,"dir-${UUID.randomUUID()}").let {
                assertEquals(RemoteDirectoryCreationState.COMPLETE,it.execute().state);it.awaitClosed();it.path
            } else null
            val entry=create(files,folder ?: parent,root)
            val target=if(folder==null)entry else files.browse(parent).entries.single { it.path==folder }
            val request=files.prepareRemoval(parent,target.name,folder!=null,
                if(folder!=null)0 else entry.size!!.toLong()+(if(change=="size")1 else 0),
                target.modifiedSeconds!!+(if(change=="time")1 else 0))
            val result=request.execute();request.awaitClosed()
            assertEquals(if(folder==null)RemoteRemovalState.CHANGED else RemoteRemovalState.REJECTED,result.state)
            assertEquals(3,result.status);if(folder!=null)assertEquals(4u,result.remoteStatus)
            files.closeAndAwait()
            val fresh=RemoteFileSession(connection(known(root)))
            try { supply(fresh);assertEquals("safe bytes ☃",fresh.previewText(entry.path)) }
            finally { fresh.closeAndAwait() }
        }
    }
    @Test fun cancelledBeforeExecutionLeavesFileAndOwnerUntouched()=real { files,parent,root ->
        val entry=create(files,parent,root)
        val request=files.prepareRemoval(parent,entry.name,false,entry.size!!.toLong(),entry.modifiedSeconds!!)
        request.cancel()
        assertTrue(runCatching { request.execute() }.exceptionOrNull() is CancellationException)
        request.awaitClosed();assertEquals(RemoteRemovalState.NOT_ATTEMPTED,request.outcome.state)
        assertEquals("safe bytes ☃",files.previewText(entry.path))
    }
    @Test fun cancellationAtEveryProtocolStageRetainsOutcomeAndAwaitsNativeRelease()=directory { root ->
        for(stage in stages)protocol(root,stage,"hold") { files,user ->
            val request=files.prepareRemoval("/fixture","child",false,4,9)
            coroutineScope {
                val pending=async(Dispatchers.IO) { request.execute() }
                await(message="removal stage must be observed") { reached(user,stage) }
                pending.cancelAndJoin()
                assertNotNull("cancelled removal must release within the calibrated bound",withTimeoutOrNull(limit()) { request.awaitClosed();true })
                assertTrue(request.finished)
                assertEquals(if(stage=="remove-delete")RemoteRemovalState.UNKNOWN else RemoteRemovalState.NOT_ATTEMPTED,request.outcome.state)
                assertEquals(8,request.outcome.status)
                val events=control("state",user).getJSONArray("events")
                val later=stages.drop(stages.indexOf(stage)+1)
                assertFalse("cancel must not submit later protocol stages",(0 until events.length()).any { events.getString(it) in later })
            }
        }
    }
    @Test fun protocolResultsKeepAbsentRejectedUnknownAndDirectoryRemovalDistinct()=directory { root ->
        data class Case(val stage:String,val action:String,val state:RemoteRemovalState,val status:Int,val remote:UInt)
        val cases=listOf(
            Case("remove-target","remove-absent",RemoteRemovalState.ABSENT,0,2u),
            Case("remove-target","remove-missing-type",RemoteRemovalState.NOT_ATTEMPTED,7,0u),
            Case("remove-target","remove-missing-size",RemoteRemovalState.NOT_ATTEMPTED,7,0u),
            Case("remove-target","remove-missing-time",RemoteRemovalState.NOT_ATTEMPTED,7,0u),
            Case("remove-parent-real","remove-alias",RemoteRemovalState.NOT_ATTEMPTED,3,0u),
            Case("remove-delete","deny",RemoteRemovalState.REJECTED,3,3u),
            Case("remove-delete","remove-unsupported",RemoteRemovalState.REJECTED,7,8u),
            Case("remove-delete","remove-unknown",RemoteRemovalState.UNKNOWN,3,222u),
            Case("remove-delete","disconnect",RemoteRemovalState.UNKNOWN,3,0u),
            Case("remove-delete","remove-malformed",RemoteRemovalState.UNKNOWN,3,0u),
            Case("remove-delete","remove-directory",RemoteRemovalState.REMOVED,0,0u))
        for(case in cases)protocol(root,case.stage,case.action) { files,user ->
            val directory=case.action=="remove-directory"
            val request=files.prepareRemoval("/fixture","child",directory,if(directory)0 else 4,9)
            val result=request.execute();request.awaitClosed()
            assertEquals(case.action,case.state,result.state);assertEquals(case.action,case.status,result.status);assertEquals(case.action,case.remote,result.remoteStatus)
            val events=control("state",user).getJSONArray("events")
            assertFalse("protocol request order/path must match",(0 until events.length()).any { events.getString(it) in listOf("remove-unexpected","remove-wrong-path") })
        }
    }
    @Test fun totalBudgetIncludesAllMetadataRequests()=directory { root ->
        protocol(root,"remove-parent-real","remove-total-budget") { files,user ->
            val request=files.prepareRemoval("/fixture","child",false,4,9,200)
            val started=SystemClock.elapsedRealtime();val result=request.execute();request.awaitClosed()
            assertEquals(RemoteRemovalState.NOT_ATTEMPTED,result.state);assertEquals(3,result.status)
            assertTrue(SystemClock.elapsedRealtime()-started>=198)
            val events=control("state",user).getJSONArray("events")
            assertFalse((0 until events.length()).any { events.getString(it)=="remove-delete" })
        }
    }
    @Test fun queuedRemovalCancellationKeepsCurrentRequestAlive()=directory { root ->
        protocol(root,"remove-delete","hold") { files,user ->
            coroutineScope {
                val first=files.prepareRemoval("/fixture","child",false,4,9)
                val current=async(Dispatchers.IO) { first.execute() }
                await { reached(user,"remove-delete") }
                val second=files.prepareRemoval("/fixture","another",false,4,9)
                val pending=launch(Dispatchers.IO,start=CoroutineStart.UNDISPATCHED) { second.execute() }
                pending.cancelAndJoin();second.awaitClosed()
                assertEquals(RemoteRemovalState.NOT_ATTEMPTED,second.outcome.state)
                assertFalse(first.finished)
                control("release",user)
                assertEquals(RemoteRemovalState.REMOVED,current.await().state)
            }
        }
    }
    private fun handle(files:RemoteFileSession):Long=RemoteFileSession::class.java.getDeclaredField("handle").let {
        it.isAccessible=true;it.getLong(files)
    }
    @Test fun removalCancellationRejectsStaleRequestsAndRecycledHandles()=directory { root ->
        val old=RemoteFileSession(connection(known(root)));supply(old)
        val stale=handle(old);old.close();await { owners()==0 }
        protocol(root,"remove-delete","hold") { files,user -> runBlocking {
            val request=files.prepareRemoval("/fixture","child",false,4,9)
            val active=async(Dispatchers.IO) { request.execute() }
            await { reached(user,"remove-delete") }
            val binding=request.javaClass.getDeclaredField("binding").let { it.isAccessible=true;it.get(request) as Pair<*,*> }
            val id=binding.second as Long
            native("cancelRemove",handle(files),id+1)
            native("cancelRemove",stale,id)
            delay(100)
            assertFalse("stale cancellation must not finish current request",request.finished)
            control("release",user)
            assertEquals(RemoteRemovalState.REMOVED,active.await().state)
        } }
    }
    @Test fun removalFenceRetainsDuplicateCloexecAndActualNativeLifetime()=directory { root ->
        protocol(root,"remove-delete","hold") { files,user ->
            val pipe=android.os.ParcelFileDescriptor.createPipe()
            try {
                val link=android.system.Os.readlink("/proc/self/fd/${pipe[1].fd}")
                val request=native("beginRemove",handle(files),"/fixture".toByteArray(),"child".toByteArray(),false,4L,9L,10000,pipe[1].fd) as Long
                await { reached(user,"remove-delete") }
                val descriptors=File("/proc/self/fd").listFiles().orEmpty().filter { runCatching {
                    android.system.Os.readlink(it.absolutePath)==link
                }.getOrDefault(false) }
                assertEquals("native completion fence must retain a duplicate",3,descriptors.size)
                descriptors.forEach { descriptor ->
                    val flags=File("/proc/self/fdinfo/"+descriptor.name).readLines().single { it.startsWith("flags:") }.substringAfter(':').trim().toLong(8)
                    assertTrue("completion descriptors must be CLOEXEC",flags and 0x80000L!=0L)
                }
                pipe[1].close()
                val poll=android.system.StructPollfd().apply { fd=pipe[0].fileDescriptor;events=android.system.OsConstants.POLLIN.toShort() }
                assertEquals("fence must not close before cancellation completes",0,android.system.Os.poll(arrayOf(poll),0))
                native("cancelRemove",handle(files),request)
                assertEquals(1,android.system.Os.poll(arrayOf(poll),limit().toInt()))
                android.os.ParcelFileDescriptor.AutoCloseInputStream(pipe[0]).use { assertEquals(-1,it.read()) }
                assertEquals(0,owners())
                val data=java.nio.ByteBuffer.wrap(native("result",handle(files),request) as ByteArray)
                assertEquals(8,data.int);assertEquals(7,data.int);assertEquals(3,data.int);assertEquals(0,data.int)
                native("finish",handle(files),request)
            } finally { pipe.forEach { it.close() } }
        }
    }

}
