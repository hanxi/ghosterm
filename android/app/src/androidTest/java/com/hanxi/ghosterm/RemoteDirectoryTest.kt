package com.hanxi.ghosterm

import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.data.LocalFileBrowser
import com.hanxi.ghosterm.data.FileBrowserRepository
import com.hanxi.ghosterm.data.FileTreeScanner
import com.hanxi.ghosterm.data.FileDestinationAccess
import com.hanxi.ghosterm.data.FilePastePreflight
import com.hanxi.ghosterm.data.TransferStagingStore
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.lang.reflect.InvocationTargetException
import java.net.Socket
import java.nio.charset.CharacterCodingException
import java.util.UUID
import kotlin.math.max

@RunWith(AndroidJUnit4::class)
class RemoteDirectoryTest {
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

    private val stages=listOf("mkdir-parent","mkdir-parent-real","mkdir-target","mkdir-create","mkdir-child",
        "mkdir-child-real","mkdir-open","mkdir-read","mkdir-eof","mkdir-close")
    private fun protocol(root:File,stage:String,action:String,block:(RemoteFileSession,String)->Unit) {
        val user=control("new",stage=stage,action=action).getString("user")
        val files=RemoteFileSession(connection(known(root,true),true,user))
        try { supply(files,true);block(files,user) }
        finally { control("release",user);files.close();await { owners()==0 } }
    }
    @Test fun realNestedDirectoriesAcceptSubsequentFilePublication()=directory { root ->
        val files=RemoteFileSession(connection(known(root)))
        try {
            supply(files)
            runBlocking {
                val parent=fixture.getString("remoteScratch")
                val request=files.prepareDirectory(parent,"目录 ☃-${UUID.randomUUID()}")
                assertNull(request.outcome.status)
                assertEquals(RemoteDirectoryCreationState.COMPLETE,request.execute().state)
                request.awaitClosed();assertTrue(request.finished)
                assertTrue("completed request must reject execution again",runCatching { request.execute() }.exceptionOrNull() is IllegalStateException)
                assertEquals(0,request.outcome.status);assertEquals(0u,request.outcome.remoteStatus)
                assertTrue(files.browse(request.path).entries.isEmpty())
                val nested=files.prepareDirectory(request.path,"empty")
                assertEquals(RemoteDirectoryCreationState.COMPLETE,nested.execute().state)
                val source=File(root,"source").apply { writeText("内容\n") }
                android.os.ParcelFileDescriptor.open(source,android.os.ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
                    assertEquals(RemoteCreationState.COMPLETE,files.createExclusive(nested.path+"/.staged",fd.fd,source.length()).state)
                }
                assertEquals(RemotePublicationState.PUBLISHED,files.publish(nested.path+"/.staged",nested.path+"/data",expectedSize=source.length()).state)
                assertEquals("内容\n",files.previewText(nested.path+"/data"))
                assertEquals(listOf("empty"),files.browse(request.path).entries.map { it.name })
            }
        } finally { files.close() }
    }
    @Test fun realConflictsPreserveFilesDirectoriesAndLinks()=directory { root ->
        for(name in listOf("文本.txt","folder","link")) {
            val files=RemoteFileSession(connection(known(root)))
            try {
                supply(files)
                runBlocking {
                    val request=files.prepareDirectory(fixture.getString("remoteRoot"),name)
                    val result=request.execute();request.awaitClosed()
                    assertEquals(RemoteDirectoryCreationState.CONFLICT,result.state);assertEquals(3,result.status)
                    assertTrue(files.state().finished);assertEquals(0,owners())
                }
            } finally { files.close() }
        }
        val verify=RemoteFileSession(connection(known(root)))
        try {
            supply(verify)
            runBlocking {
                assertEquals("远端 UTF-8\nline two\n",verify.previewText(fixture.getString("remoteRoot")+"/文本.txt"))
                val entries=verify.browse().entries.associateBy { it.name }
                assertTrue(entries.getValue("folder").directory);assertTrue(entries.getValue("link").symbolicLink)
            }
        } finally { verify.close() }
    }
    @Test fun invalidAndCancelledRequestsDoNotConsumeConnection()=directory { root ->
        val files=RemoteFileSession(connection(known(root)))
        try {
            supply(files)
            for(parent in listOf("relative","//double","/trailing/","/./dot","/foo/../bar"))
                assertTrue(runCatching { files.prepareDirectory(parent,"child") }.isFailure)
            for(name in listOf("",".","..","two/parts","bad\u0000name","界".repeat(86),"\uD800"))
                assertTrue(runCatching { files.prepareDirectory("/",name) }.isFailure)
            assertTrue(runCatching { files.prepareDirectory("/"+"x".repeat(65534),"child") }.isFailure)
            assertTrue(runCatching { files.prepareDirectory("/","child",0) }.isFailure)
            assertTrue(runCatching { files.prepareDirectory("/","child",60001) }.isFailure)
            runBlocking {
                val request=files.prepareDirectory(fixture.getString("remoteScratch"),"cancel-before-${UUID.randomUUID()}")
                request.cancel();assertTrue(runCatching { request.execute() }.exceptionOrNull() is CancellationException)
                request.awaitClosed();assertEquals(RemoteDirectoryCreationState.NOT_ATTEMPTED,request.outcome.state)
                assertEquals(8,request.outcome.status)
                assertTrue(runCatching { request.execute() }.isFailure)
                assertFalse(files.browse().entries.any { it.name==request.name })
            }
        } finally { files.close() }
    }
    @Test fun protocolStatesAndRawErrorsSurviveFailures()=directory { root ->
        for(stage in listOf("mkdir-target","mkdir-create","mkdir-child","mkdir-close")) {
            for(action in listOf("deny","mkdir-unknown","mkdir-lost","mkdir-unsupported","mkdir-race","mkdir-malformed")) {
                protocol(root,stage,action) { files,_ -> runBlocking {
                    val request=files.prepareDirectory("/parent","child")
                    val result=request.execute();request.awaitClosed()
                    val state=if(stage=="mkdir-target") RemoteDirectoryCreationState.NOT_ATTEMPTED
                        else if(stage!="mkdir-create") RemoteDirectoryCreationState.CREATED
                        else if(action in listOf("deny","mkdir-unsupported","mkdir-race")) RemoteDirectoryCreationState.REJECTED
                        else RemoteDirectoryCreationState.UNKNOWN
                    val remote=when(action){"deny"->3u;"mkdir-unknown"->0xdeadbeefu;"mkdir-lost"->7u;"mkdir-unsupported"->8u;"mkdir-race"->4u;else->0u}
                    assertEquals("$stage/$action",state,result.state);assertEquals(remote,result.remoteStatus)
                    assertEquals(if(stage=="mkdir-create" && action=="mkdir-unsupported") 7 else 3,result.status)
                    assertEquals(request.outcome,result);assertTrue(request.finished);assertEquals(0,owners())
                    assertTrue("failed request must release its registered connection",runCatching { files.authentication() }.isFailure)
                } }
            }
        }
    }
    @Test fun metadataAndEmptyDirectoryVerificationRejectUncertainTargets()=directory { root ->
        for((stage,action) in listOf("mkdir-parent" to "mkdir-missing-type","mkdir-parent" to "mkdir-link",
            "mkdir-parent-real" to "mkdir-alias","mkdir-child" to "mkdir-file","mkdir-child-real" to "mkdir-alias",
            "mkdir-read" to "mkdir-nonempty","mkdir-read" to "mkdir-duplicate-dot")) {
            protocol(root,stage,action) { files,user -> runBlocking {
                val request=files.prepareDirectory("/parent","child")
                val result=request.execute();request.awaitClosed()
                assertNotEquals(0,result.status)
                assertEquals(if(stages.indexOf(stage)<3) RemoteDirectoryCreationState.NOT_ATTEMPTED else RemoteDirectoryCreationState.CREATED,result.state)
                if(stages.indexOf(stage)<3)assertFalse(reached(user,"mkdir-create"))
                assertFalse(reached(user,"mkdir-close"))
            } }
        }
    }
    @Test fun oneDeadlineCoversEveryProtocolStage()=directory { root ->
        val bound=limit()
        for(stage in stages) protocol(root,stage,"hold") { files,_ -> runBlocking {
            val request=files.prepareDirectory("/parent","child",400)
            val start=SystemClock.uptimeMillis();val result=request.execute();request.awaitClosed()
            val elapsed=SystemClock.uptimeMillis()-start
            assertTrue("deadline stage $stage elapsed $elapsed",elapsed in 398 until bound)
            assertEquals(3,result.status)
            assertEquals(if(stages.indexOf(stage)<3) RemoteDirectoryCreationState.NOT_ATTEMPTED
                else if(stage=="mkdir-create") RemoteDirectoryCreationState.UNKNOWN else RemoteDirectoryCreationState.CREATED,result.state)
            assertEquals(0,owners())
        } }
        protocol(root,"mkdir-close","mkdir-total-budget") { files,user -> runBlocking {
            val request=files.prepareDirectory("/parent","child",400)
            assertEquals(3,request.execute().status);request.awaitClosed()
            assertTrue(reached(user,"mkdir-parent-real"));assertFalse(reached(user,"mkdir-target"))
        } }
    }
    @Test fun cancellationRetainsStageOutcomeUntilActualOwnerRelease()=directory { root ->
        val bound=limit()
        for(stage in stages) protocol(root,stage,"hold") { files,user -> runBlocking {
            val request=files.prepareDirectory("/parent","child",10000)
            val waiting=launch(Dispatchers.IO) { request.execute() }
            await { reached(user,stage) }
            val start=SystemClock.uptimeMillis();waiting.cancelAndJoin();request.awaitClosed()
            assertTrue(SystemClock.uptimeMillis()-start<bound)
            assertEquals(8,request.outcome.status)
            assertEquals(if(stages.indexOf(stage)<3) RemoteDirectoryCreationState.NOT_ATTEMPTED
                else if(stage=="mkdir-create") RemoteDirectoryCreationState.UNKNOWN else RemoteDirectoryCreationState.CREATED,request.outcome.state)
            assertTrue(request.finished);assertEquals(0,owners())
        } }
    }
    @Test fun lateCancellationCannotEraseVerifiedCreation()=directory { root ->
        protocol(root,"mkdir-close","hold") { files,user -> runBlocking {
            val pending=java.util.concurrent.ConcurrentLinkedQueue<Runnable>()
            val dispatcher=object:CoroutineDispatcher() {
                override fun dispatch(context:kotlin.coroutines.CoroutineContext,block:Runnable) { pending.add(block) }
            }
            val request=files.prepareDirectory("/parent","child")
            val caller=async(dispatcher,start=CoroutineStart.UNDISPATCHED) { request.execute() }
            await { reached(user,"mkdir-close") };control("release",user)
            request.awaitClosed()
            assertEquals(RemoteDirectoryCreationState.COMPLETE,request.outcome.state);assertEquals(0,request.outcome.status)
            assertFalse(caller.isCompleted)
            // Completion can be published just before the suspended caller's
            // continuation is enqueued. Freeze that boundary before cancelling.
            await { pending.isNotEmpty() }
            caller.cancel()
            await { while(pending.isNotEmpty()) pending.poll()?.run();caller.isCompleted }
            caller.join()
            assertEquals(RemoteDirectoryCreationState.COMPLETE,request.outcome.state)
            assertTrue(runCatching { request.execute() }.isFailure)
        } }
    }
    @Test fun cancellingQueuedRequestDoesNotCancelCurrentOperation()=directory { root ->
        protocol(root,"mkdir-close","hold") { files,user -> runBlocking {
            val first=files.prepareDirectory("/parent","child")
            val current=async(Dispatchers.IO) { first.execute() }
            await { reached(user,"mkdir-close") }
            val second=files.prepareDirectory("/parent","another")
            val queued=launch(Dispatchers.IO,start=CoroutineStart.UNDISPATCHED) { second.execute() }
            queued.cancelAndJoin();second.awaitClosed()
            assertEquals(RemoteDirectoryCreationState.NOT_ATTEMPTED,second.outcome.state)
            assertFalse(first.finished)
            control("release",user)
            assertEquals(RemoteDirectoryCreationState.COMPLETE,current.await().state)
        } }
    }
    @Test fun closingSessionDuringCreationKeepsUncertainResultAndReleasesRequest()=directory { root ->
        protocol(root,"mkdir-create","hold") { files,user -> runBlocking {
            val request=files.prepareDirectory("/parent","child")
            val current=async(Dispatchers.IO) { request.execute() }
            await { reached(user,"mkdir-create") };files.close()
            current.await();request.awaitClosed()
            assertEquals(RemoteDirectoryCreationState.UNKNOWN,request.outcome.state)
            assertNotEquals(0,request.outcome.status);assertEquals(0,owners())
        } }
    }
    private fun handle(files:RemoteFileSession):Long=RemoteFileSession::class.java.getDeclaredField("handle").let {
        it.isAccessible=true;it.getLong(files)
    }
    @Test fun nativeCancellationRejectsStaleRequestsAndRecycledHandles()=directory { root ->
        val old=RemoteFileSession(connection(known(root)));supply(old)
        val stale=handle(old);old.close();await { owners()==0 }
        protocol(root,"mkdir-close","hold") { files,user -> runBlocking {
            val request=files.prepareDirectory("/parent","child")
            val active=async(Dispatchers.IO) { request.execute() }
            await { reached(user,"mkdir-close") }
            val binding=request.javaClass.getDeclaredField("binding").let { it.isAccessible=true;it.get(request) as Pair<*,*> }
            val id=binding.second as Long
            native("cancelMkdir",handle(files),id+1)
            native("cancelMkdir",stale,id)
            delay(100)
            assertFalse("stale cancellation must not finish current request",request.finished)
            control("release",user)
            assertEquals(RemoteDirectoryCreationState.COMPLETE,active.await().state)
        } }
    }
    @Test fun nativeFenceRemainsOwnedUntilCancelledConnectionActuallyCloses()=directory { root ->
        protocol(root,"mkdir-child","hold") { files,user ->
            val pipe=android.os.ParcelFileDescriptor.createPipe()
            try {
                val link=android.system.Os.readlink("/proc/self/fd/${pipe[1].fd}")
                val request=native("beginMkdir",handle(files),"/parent".toByteArray(),"child".toByteArray(),10000,pipe[1].fd) as Long
                await { reached(user,"mkdir-child") }
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
                native("cancelMkdir",handle(files),request)
                assertEquals(1,android.system.Os.poll(arrayOf(poll),limit().toInt()))
                android.os.ParcelFileDescriptor.AutoCloseInputStream(pipe[0]).use { assertEquals(-1,it.read()) }
                assertEquals(0,owners())
                val data=java.nio.ByteBuffer.wrap(native("result",handle(files),request) as ByteArray)
                assertEquals(8,data.int);assertEquals(6,data.int);assertEquals(4,data.int);assertEquals(0,data.int)
                native("finish",handle(files),request)
            } finally { pipe.forEach { it.close() } }
        }
    }

}
