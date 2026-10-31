package com.hanxi.ghosterm

import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.data.FileDirectoryCreator
import com.hanxi.ghosterm.data.RemoteFileBinding
import com.hanxi.ghosterm.data.FileDirectoryCreator.State
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
class RemoteDirectoryDataTest {
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
    private fun endpoint(files: RemoteFileSession) = FileEndpoint("remote:${UUID.randomUUID()}","frozen",".",
        remote=RemoteFileIdentity("frozen",files.peer.host,files.peer.user,files.peer.port))
    private fun creator(root: File, binding: RemoteFileBinding) = FileDirectoryCreator(root,
        instrumentation.targetContext.contentResolver) { binding }
    private suspend fun rejected(block: suspend () -> Unit) { assertTrue("must reject before IO",runCatching { block() }.isFailure) }

    @Test fun unifiedCreationKeepsFrozenLocationsAndSupportsNestedFilePublication()=directory { root ->
        val files=RemoteFileSession(connection(known(root)))
        try { supply(files);runBlocking {
            val endpoint=endpoint(files);val binding=RemoteFileBinding(endpoint,files);val create=creator(root,binding)
            val parent=FileLocation(endpoint,fixture.getString("remoteRoot"))
            val name="统一目录 ☃-${UUID.randomUUID()}"
            val request=create.prepare(parent,name)
            val result=request.execute();request.awaitClosed()
            assertEquals(State.COMPLETE,result.state);assertTrue(request.finished)
            assertEquals(parent.copy(directoryId=parent.directoryId+"/"+name),result.location)
            assertEquals(name,result.actualName);assertNull(result.documentUri);assertNull(result.error)
            assertEquals(0,result.coreStatus);assertEquals(0u,result.remoteStatus)
            rejected { request.execute() }
            val child=create.prepare(checkNotNull(result.location),"child").execute()
            assertEquals(State.COMPLETE,child.state)
            val path=checkNotNull(child.location).directoryId
            val content=File(root,"source").apply { writeText("冻结后的真实文件 ☃\n") }
            android.os.ParcelFileDescriptor.open(content,android.os.ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
                assertEquals(RemoteCreationState.COMPLETE,files.createExclusive(path+"/.candidate",fd.fd,content.length()).state)
            }
            assertEquals(RemotePublicationState.PUBLISHED,files.publish(path+"/.candidate",path+"/data",expectedSize=content.length()).state)
            assertEquals(content.readText(),files.previewText(path+"/data"))
            assertEquals(listOf("child"),files.browse(checkNotNull(result.location).directoryId).entries.map { it.name })
            assertEquals(State.COMPLETE,create.prepare(FileLocation(FileEndpoint.APP),"local").execute().state)
            assertTrue(File(root,"local").isDirectory)
        } } finally { files.close() }
    }
    @Test fun peerAndEndpointMismatchesAreRejectedBeforeAnyCreation()=directory { root ->
        val files=RemoteFileSession(connection(known(root)))
        try { supply(files);runBlocking {
            val original=endpoint(files);val identity=checkNotNull(original.remote)
            assertEquals(host,files.peer.host);assertEquals(fixture.getInt("port"),files.peer.port)
            assertEquals(fixture.getString("user"),files.peer.user)
            for(changed in listOf(identity.copy(host="other.example"),identity.copy(user="other-user"),identity.copy(port=1)))
                rejected { RemoteFileBinding(original.copy(remote=changed),files) }
            rejected { RemoteFileBinding(original.copy(treeUri="content://invalid/tree/root"),files) }
            rejected { RemoteFileBinding(FileEndpoint.APP,files) }
            rejected { RemoteFileBinding(original.copy(id=""),files) }
            val create=creator(root,RemoteFileBinding(original,files))
            for(changed in listOf(original.copy(id="remote:other"),original.copy(label="changed"),original.copy(rootId="/other"),
                original.copy(remote=identity.copy(alias="changed"))))
                rejected { create.prepare(FileLocation(changed,fixture.getString("remoteRoot")),"not-created") }
            assertFalse(files.browse(fixture.getString("remoteRoot")).entries.any { it.name=="not-created" })
            files.close()
            rejected { create.prepare(FileLocation(original,"/parent"),"not-created") }
        } } finally { files.close() }
    }
    @Test fun invalidRemoteInputsAndPreCancellationNeverCreateOrCloseTheConnection()=directory { root ->
        val files=RemoteFileSession(connection(known(root)))
        try { supply(files);runBlocking {
            val endpoint=endpoint(files);val location=FileLocation(endpoint,fixture.getString("remoteRoot"));val create=creator(root,RemoteFileBinding(endpoint,files))
            rejected { FileDirectoryCreator(root,instrumentation.targetContext.contentResolver).prepare(location,"absent") }
            for(path in listOf("relative","//bad","/parent/..","/parent/","/parent/./child"))
                rejected { create.prepare(location.copy(directoryId=path),"absent") }
            for(budget in listOf(0,60001)) rejected { create.prepare(location,"absent",budget) }
            for(name in listOf("","..","a/b","bad\u0000name","界".repeat(86),"\ud800")) rejected { create.prepare(location,name) }
            val name="cancelled-${UUID.randomUUID()}";val request=create.prepare(location,name)
            request.cancel();request.awaitClosed()
            val waiting=launch { request.execute();fail("cancelled request delivered") };waiting.join()
            assertTrue(waiting.isCancelled);assertEquals(State.NOT_ATTEMPTED,request.outcome.state)
            assertNull(request.outcome.location);assertNull(request.outcome.coreStatus)
            assertFalse(files.browse(location.directoryId).entries.any { it.name==name })
        } } finally { files.close() }
    }
    @Test fun eachRemoteStateAndRawStatusSurvivesTheDataBoundary()=directory { root ->
        data class Case(val stage:String,val action:String,val state:State,val status:Int,val remote:UInt)
        val cases=listOf(Case("mkdir-parent","deny",State.NOT_ATTEMPTED,3,3u),
            Case("mkdir-target","mkdir-conflict",State.CONFLICT,3,0u),Case("mkdir-create","deny",State.REJECTED,3,3u),
            Case("mkdir-create","mkdir-unknown",State.UNKNOWN,3,0xdeadbeefu),
            Case("mkdir-child","deny",State.CREATED,3,3u),Case("mkdir-create","mkdir-unsupported",State.REJECTED,7,8u),
            Case("mkdir-close","allow",State.COMPLETE,0,0u))
        for(case in cases) protocol(root,case.stage,case.action) { files,_ -> runBlocking {
            val endpoint=endpoint(files);val parent=FileLocation(endpoint,"/parent")
            val request=creator(root,RemoteFileBinding(endpoint,files)).prepare(parent,"child")
            val result=request.execute();request.awaitClosed()
            assertEquals(case.state,result.state);assertEquals(case.status,result.coreStatus);assertEquals(case.remote,result.remoteStatus)
            assertNull(result.documentUri)
            val created=case.state==State.CREATED || case.state==State.COMPLETE
            assertEquals(if(created) parent.copy(directoryId="/parent/child") else null,result.location)
            assertEquals(if(created) "child" else null,result.actualName)
            assertEquals(case.status!=0,result.error!=null)
            assertEquals(parent,request.parent);assertEquals("child",request.requestedName)
            if(case.status!=0) { assertEquals(0,owners());assertTrue(files.state().finished) }
        } }
    }
    @Test fun cancellationWaitsForFinalNativeOutcomeAndPreservesCreatedLocation()=directory { root ->
        val bound=limit()
        for(stage in listOf("mkdir-create","mkdir-child")) protocol(root,stage,"hold") { files,user -> runBlocking {
            val parent=FileLocation(endpoint(files),"/parent")
            val request=creator(root,RemoteFileBinding(parent.endpoint,files)).prepare(parent,"child",10000)
            val waiting=launch(Dispatchers.IO) { request.execute() }
            await { reached(user,stage) }
            val start=SystemClock.uptimeMillis();waiting.cancelAndJoin();request.awaitClosed()
            assertTrue(SystemClock.uptimeMillis()-start<bound)
            assertEquals(if(stage=="mkdir-create") State.UNKNOWN else State.CREATED,request.outcome.state)
            assertEquals(8,request.outcome.coreStatus);assertEquals(0u,request.outcome.remoteStatus)
            assertEquals(if(stage=="mkdir-child") parent.copy(directoryId="/parent/child") else null,request.outcome.location)
            assertNotNull(request.outcome.error);assertTrue(request.finished);assertEquals(0,owners())
        } }
    }
    @Test fun verifiedCreationSurvivesCancellationOfWaitingCaller()=directory { root ->
        protocol(root,"mkdir-close","hold") { files,user -> runBlocking {
            val pending=java.util.concurrent.ConcurrentLinkedQueue<Runnable>()
            val dispatcher=object:CoroutineDispatcher() {
                override fun dispatch(context:kotlin.coroutines.CoroutineContext,block:Runnable) { pending.add(block) }
            }
            val parent=FileLocation(endpoint(files),"/parent")
            val request=creator(root,RemoteFileBinding(parent.endpoint,files)).prepare(parent,"child")
            val caller=async(dispatcher,start=CoroutineStart.UNDISPATCHED) { request.execute() }
            await { reached(user,"mkdir-close") };control("release",user)
            request.awaitClosed();assertEquals(State.COMPLETE,request.outcome.state);assertFalse(caller.isCompleted)
            caller.cancel();while(pending.isNotEmpty())pending.poll()?.run();caller.join()
            assertEquals(State.COMPLETE,request.outcome.state);assertEquals(0,request.outcome.coreStatus)
            assertEquals(parent.copy(directoryId="/parent/child"),request.outcome.location);assertNull(request.outcome.error)
        } }
    }
    @Test fun bindingIsCapturedOnceAndBrowserClosureCannotRetargetTheRequest()=directory { root ->
        val files=RemoteFileSession(connection(known(root)));val browser=RemoteFileSession(connection(known(root)))
        val repository=FileBrowserRepository(LocalFileBrowser(File(root,"home"),instrumentation.targetContext.contentResolver))
        try { supply(files);supply(browser);runBlocking {
            val identity=RemoteFileIdentity("same-alias",files.peer.host,files.peer.user,files.peer.port)
            val endpoint=repository.attach(identity,browser)
            var current=RemoteFileBinding(endpoint,files);var lookups=0
            val create=FileDirectoryCreator(root,instrumentation.targetContext.contentResolver) { lookups++;current }
            val location=FileLocation(endpoint,fixture.getString("remoteRoot"))
            val request=create.prepare(location,"frozen-${UUID.randomUUID()}")
            current=RemoteFileBinding(endpoint.copy(id="replacement",label="replacement"),browser)
            repository.close()
            assertEquals(1,lookups)
            val result=request.execute();request.awaitClosed()
            assertEquals(State.COMPLETE,result.state);assertEquals(1,lookups)
            assertEquals(endpoint,result.location?.endpoint);assertTrue(files.state().ready)
            assertTrue(files.browse(checkNotNull(result.location).directoryId).entries.isEmpty())
        } } finally { repository.close();browser.close();files.close() }
    }
    @Test fun cancellationAfterNativeAcknowledgementKeepsVerifiedResult()=directory { root ->
        protocol(root,"mkdir-close","hold") { files,user -> runBlocking {
            val parent=FileLocation(endpoint(files),"/parent")
            val request=creator(root,RemoteFileBinding(parent.endpoint,files)).prepare(parent,"child")
            val inner=request.javaClass.getDeclaredField("remoteRequest").apply { isAccessible=true }
                .get(request) as RemoteFileSession.DirectoryRequest
            val waiting=launch(Dispatchers.IO) { request.execute() }
            await { reached(user,"mkdir-close") };control("release",user)
            await { inner.outcome.state==RemoteDirectoryCreationState.COMPLETE }
            waiting.cancelAndJoin();request.awaitClosed()
            assertEquals(State.COMPLETE,request.outcome.state);assertEquals(0,request.outcome.coreStatus)
            assertEquals(parent.copy(directoryId="/parent/child"),request.outcome.location)
            assertNull("cancelled caller must not rewrite verified result",request.outcome.error)
        } }
    }
    @Test fun remoteBudgetIsForwardedAndQueuedCancellationIsIsolated()=directory { root ->
        val bound=limit()
        protocol(root,"mkdir-child","hold") { files,_ -> runBlocking {
            val endpoint=endpoint(files);val request=creator(root,RemoteFileBinding(endpoint,files)).prepare(FileLocation(endpoint,"/parent"),"child",400)
            val start=SystemClock.uptimeMillis();assertEquals(State.CREATED,request.execute().state);request.awaitClosed()
            assertTrue(SystemClock.uptimeMillis()-start in 398 until bound);assertEquals(3,request.outcome.coreStatus)
        } }
        protocol(root,"mkdir-close","hold") { files,user -> runBlocking {
            val endpoint=endpoint(files);val create=creator(root,RemoteFileBinding(endpoint,files));val parent=FileLocation(endpoint,"/parent")
            val first=create.prepare(parent,"child");val running=async(Dispatchers.IO) { first.execute() }
            await { reached(user,"mkdir-close") }
            val second=create.prepare(parent,"another");val queued=launch(Dispatchers.IO,start=CoroutineStart.UNDISPATCHED) { second.execute() }
            queued.cancelAndJoin();second.awaitClosed()
            assertEquals(State.NOT_ATTEMPTED,second.outcome.state);assertNull(second.outcome.location)
            control("release",user);assertEquals(State.COMPLETE,running.await().state);first.awaitClosed()
        } }
    }
}
