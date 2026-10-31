package com.hanxi.ghosterm

import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.data.FileTaskConnections
import com.hanxi.ghosterm.data.FileTaskConnections.Role
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
import java.util.UUID
import kotlin.math.max

@RunWith(AndroidJUnit4::class)
class RemoteFileTaskTest {
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

    private fun endpoint(id: String = UUID.randomUUID().toString(), protocol: Boolean = false,
                         user: String = fixture.getString("user")) = FileEndpoint(id,"frozen",".",
        remote=RemoteFileIdentity("frozen",host,user,if(protocol) peer.getInt("port") else fixture.getInt("port")))
    private fun task(root: File, source: FileEndpoint, destination: FileEndpoint,
                     auth: suspend (Role,FileEndpoint,RemoteFileSession)->Unit = { _,_,session -> supply(session) }) =
        FileTaskConnections(source,destination,{ _,_ -> connection(known(root)) },auth)
    private suspend fun rejected(block: suspend () -> Unit) {
        assertTrue("operation must reject",runCatching { block() }.isFailure)
    }
    private fun pipes(): Map<String,String> = File("/proc/self/fd").listFiles().orEmpty().mapNotNull { fd ->
        runCatching { android.system.Os.readlink(fd.path) }.getOrNull()?.takeIf { it.startsWith("pipe:") }?.let { fd.name to it }
    }.toMap()

    @Test fun closeWaitsForItsOwnOwnerAndPreservesAnotherConnection()=directory { root -> runBlocking {
        val browser=RemoteFileSession(connection(known(root)))
        val files=RemoteFileSession(connection(known(root)))
        try {
            supply(browser);supply(files);assertEquals(2,owners())
            val bound=limit();val start=SystemClock.uptimeMillis()
            withTimeout(bound) { coroutineScope { repeat(4) { launch { files.closeAndAwait() } } } }
            assertTrue(SystemClock.uptimeMillis()-start<bound)
            assertEquals("await must include native owner exit",1,owners())
            assertTrue(files.state().finished)
            assertEquals(fixture.getString("remoteRoot"),browser.browse(fixture.getString("remoteRoot")).path)
            files.closeAndAwait()
        } finally { files.closeAndAwait();browser.closeAndAwait() }
    } }

    @Test fun closeDuringCredentialsAndSftpSetupReleasesResources()=directory { root -> runBlocking {
        val files=RemoteFileSession(connection(known(root)))
        phase(files,SSHAuthPhase.CREDENTIALS)
        withTimeout(limit()) { files.closeAndAwait() }
        assertEquals(0,owners())
        val user=control("new",stage="version",action="hold").getString("user")
        val opening=RemoteFileSession(connection(known(root,true),true,user))
        try {
            supply(opening,true,false);await { reached(user,"version") }
            withTimeout(limit()) { opening.closeAndAwait() }
            assertEquals(0,owners())
        } finally { control("release",user);opening.closeAndAwait() }
    } }

    @Test fun cancelledWaiterDoesNotPoisonRepeatedCloseOrRecycledSlot()=directory { root -> runBlocking {
        val first=RemoteFileSession(connection(known(root)))
        supply(first)
        val waiting=launch(start=CoroutineStart.UNDISPATCHED) { first.closeAndAwait() }
        waiting.cancelAndJoin()
        val next=RemoteFileSession(connection(known(root)))
        try {
            supply(next);withTimeout(limit()) { first.closeAndAwait() }
            assertEquals(1,owners());assertTrue(next.state().ready)
            assertTrue(next.browse(fixture.getString("remoteRoot")).path.startsWith('/'))
        } finally { first.closeAndAwait();next.closeAndAwait() }
    } }

    @Test fun completionPipeIsCloexecAndCloseWithoutWaiterDrainsIt()=directory { root -> runBlocking {
        val files=RemoteFileSession(connection(known(root)))
        try {
            supply(files)
            val field=files.javaClass.getDeclaredField("completion").apply { isAccessible=true }
            val descriptors=field.get(files) as Array<*>
            val reader=descriptors[0] as android.os.ParcelFileDescriptor
            val inode=android.system.Os.readlink("/proc/self/fd/${reader.fd}")
            val matches=pipes().filterValues { it==inode }
            assertEquals("one reader plus native duplicate",2,matches.size)
            for(fd in matches.keys) {
                val flags=File("/proc/self/fdinfo/$fd").readLines().single { it.startsWith("flags:") }.substringAfter(':').trim().toLong(8)
                assertTrue("completion FD must not reach future PTY children",flags and android.system.OsConstants.O_CLOEXEC.toLong()!=0L)
            }
            files.close()
            await { pipes().values.none { it==inode } }
            assertEquals(0,owners())
        } finally { files.closeAndAwait() }
    } }

    @Test fun connectionCompletionIncludesPendingDirectoryFence()=directory { root -> runBlocking {
        val user=control("new",stage="mkdir-child",action="hold").getString("user")
        val files=RemoteFileSession(connection(known(root,true),true,user))
        val requestPipe=android.os.ParcelFileDescriptor.createPipe()
        try {
            supply(files,true)
            val handle=files.javaClass.getDeclaredField("handle").apply { isAccessible=true }.getLong(files)
            val request=native("beginMkdir",handle,"/parent".toByteArray(),"child".toByteArray(),10000,requestPipe[1].fd) as Long
            assertTrue(request>0);requestPipe[1].close()
            await { reached(user,"mkdir-child") }
            withTimeout(limit()) { files.closeAndAwait() }
            val poll=android.system.StructPollfd().apply {
                fd=requestPipe[0].fileDescriptor;events=android.system.OsConstants.POLLIN.toShort()
            }
            android.system.Os.poll(arrayOf(poll),0)
            assertTrue("owner completion must include closing the pending request writer",
                poll.revents.toInt() and android.system.OsConstants.POLLHUP!=0)
            assertEquals(0,owners())
        } finally {
            control("release",user);files.closeAndAwait();requestPipe.forEach { it.close() }
        }
    } }

    @Test fun constructionFailureAndFullRegistryDoNotLeakNotificationPipes()=directory { root -> runBlocking {
        val known=known(root)
        val held=mutableListOf<RemoteFileSession>()
        try {
            repeat(16) { held.add(RemoteFileSession(connection(known))) }
            held.forEach { phase(it,SSHAuthPhase.CREDENTIALS) }
            val before=pipes()
            repeat(4) { rejected { RemoteFileSession(connection(known)) } }
            assertEquals("full registry must close both notification ends",before,pipes())
            repeat(4) { rejected { RemoteFileSession(ResolvedSSHConnection(host,0,fixture.getString("user"),known.path,byteArrayOf(127,0,0,1))) } }
            assertEquals("invalid construction must close both notification ends",before,pipes())
            rejected { native("create",host.toByteArray(),fixture.getInt("port"),fixture.getString("user").toByteArray(),
                known.path.toByteArray(),arrayOf(byteArrayOf(127,0,0,1)),intArrayOf(0),10000,-1) }
            assertEquals(before,pipes());assertEquals(16,owners())
        } finally { held.forEach { it.close() };held.forEach { it.closeAndAwait() } }
    } }

    @Test fun separateTaskRolesPublishRealBytesAndLeaveBrowserAlive()=directory { root -> runBlocking {
        val browser=RemoteFileSession(connection(known(root)))
        val source=endpoint();val destination=endpoint()
        val original=File(root,"content").apply { writeText("任务独立连接 ☃\n".repeat(512)) }
        val remote=fixture.getString("remoteScratch")+"/task-${UUID.randomUUID()}"
        val copy=remote+"-copy"
        var escaped: FileTaskConnections.Scope?=null
        try {
            supply(browser)
            android.os.ParcelFileDescriptor.open(original,android.os.ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
                assertEquals(RemoteCreationState.COMPLETE,browser.createExclusive(remote,fd.fd,original.length()).state)
            }
            task(root,source,destination).use { connections ->
                escaped=connections
                val from=checkNotNull(connections.remote(Role.SOURCE,source)).session
                val to=checkNotNull(connections.remote(Role.DESTINATION,destination)).session
                assertNotSame(from,to);assertNotSame(browser,from);assertNotSame(browser,to)
                assertEquals(3,owners())
                rejected { connections.remote(Role.SOURCE,destination) }
                rejected { connections.remote(Role.DESTINATION,destination.copy(label="edited")) }
                TransferStagingStore(File(root,"staging")).use { store ->
                    store.receive(from,remote,original.length()).use { snapshot ->
                        snapshot.withDescriptor { fd,size ->
                            assertEquals(RemoteCreationState.COMPLETE,to.createExclusive(copy+".candidate",fd,size).state)
                            assertEquals(RemotePublicationState.PUBLISHED,to.publish(copy+".candidate",copy,expectedSize=size).state)
                        }
                    }
                    assertEquals(0L,store.reservedBytes)
                }
                assertEquals(original.readText(),to.previewText(copy))
            }
            assertEquals("task must release both owners",1,owners())
            rejected { checkNotNull(escaped).remote(Role.SOURCE,source) }
            assertEquals(original.readText(),browser.previewText(copy))
        } finally { browser.closeAndAwait() }
    } }

    @Test fun sameEndpointStillGetsTwoOwnersAndClosedRoleIsNotReconnected()=directory { root -> runBlocking {
        val endpoint=endpoint();var resolutions=0
        val task=FileTaskConnections(endpoint,endpoint,{ _,_->resolutions++;connection(known(root)) },{ _,_,s->supply(s) })
        task.use { scope ->
            val source=checkNotNull(scope.remote(Role.SOURCE,endpoint)).session
            val destination=checkNotNull(scope.remote(Role.DESTINATION,endpoint)).session
            assertNotSame(source,destination);assertEquals(2,resolutions)
            source.closeAndAwait()
            rejected { scope.remote(Role.SOURCE,endpoint) }
            assertSame(destination,scope.remote(Role.DESTINATION,endpoint)?.session)
            assertTrue(destination.browse(fixture.getString("remoteRoot")).path.startsWith('/'))
            assertEquals(2,resolutions)
        }
        assertEquals(0,owners())
        rejected { task.use { fail("task must be one shot") } }
    } }

    @Test fun localEndpointsNeverCreateConnectionsAndScopeCannotEscape()=runBlocking {
        val source=FileEndpoint.APP;val target=FileEndpoint("saf:tree","tree","opaque",treeUri="content://provider/tree/opaque")
        val task=FileTaskConnections(source,target,{ _,_->error("local resolve") },{ _,_,_->error("local authenticate") })
        lateinit var escaped:FileTaskConnections.Scope
        assertEquals(42,task.use { scope ->
            escaped=scope;assertNull(scope.remote(Role.SOURCE,source));assertNull(scope.remote(Role.DESTINATION,target))
            rejected { scope.remote(Role.SOURCE,target) };42
        })
        rejected { escaped.remote(Role.SOURCE,source) };rejected { task.use {} }
        assertEquals(0,owners())
    }

    @Test fun resolvedIdentityChangesAreRejectedBeforeAuthentication()=directory { root -> runBlocking {
        val endpoint=endpoint();val identity=checkNotNull(endpoint.remote)
        val changed=listOf(identity.copy(host="changed.example"),identity.copy(user="changed-user"),identity.copy(port=1))
        for(other in changed) {
            var authenticated=false
            val task=FileTaskConnections(endpoint,FileEndpoint.APP,{ _,_->
                ResolvedSSHConnection(other.host,other.port,other.user,known(root).path,byteArrayOf(127,0,0,1))
            },{ _,_,_->authenticated=true;error("changed identity reached authentication") })
            rejected { task.use { fail("changed identity reached task") } }
            assertFalse(authenticated);assertEquals(0,owners())
        }
        for(other in listOf(endpoint.copy(treeUri="content://mixed"),endpoint.copy(id=""),
            endpoint.copy(remote=identity.copy(user="")),endpoint.copy(remote=identity.copy(port=0))))
            rejected { task(root,FileEndpoint.APP,other) }
    } }

    @Test fun secondEndpointFailureReleasesFirstAndPreservesOriginalError()=directory { root -> runBlocking {
        for(failAuth in listOf(false,true)) {
            val sentinel=IllegalStateException("second endpoint failure")
            val task=FileTaskConnections(endpoint(),endpoint(),{ role,_->
                if(role==Role.DESTINATION && !failAuth) throw sentinel
                connection(known(root))
            },{ role,_,session->if(role==Role.DESTINATION) throw sentinel else supply(session) })
            val result=runCatching { task.use { fail("failed connection entered task") } }
            assertSame(sentinel,result.exceptionOrNull());assertEquals("cleanup must await all acquired owners",0,owners())
        }
    } }

    @Test fun cancellationOfLateResolverCannotAcquireAnotherOwner()=directory { root -> runBlocking {
        val entered=CompletableDeferred<Unit>();val release=CompletableDeferred<Unit>();var authentications=0
        val task=FileTaskConnections(endpoint(),endpoint(),{ role,_->
            if(role==Role.DESTINATION) { entered.complete(Unit);withContext(NonCancellable) { release.await() } }
            connection(known(root))
        },{ _,_,session->authentications++;supply(session) })
        val waiting=launch { task.use { fail("cancelled resolver entered task") } }
        entered.await();waiting.cancel();release.complete(Unit);waiting.join()
        assertTrue(waiting.isCancelled);assertEquals(1,authentications);assertEquals(0,owners())
    } }

    @Test fun cancellationDuringAuthenticationWaitsForAllAcquiredOwners()=directory { root -> runBlocking {
        val entered=CompletableDeferred<Unit>()
        val task=task(root,endpoint(),endpoint()) { role,_,session ->
            if(role==Role.SOURCE) supply(session)
            else { phase(session,SSHAuthPhase.CREDENTIALS);entered.complete(Unit);awaitCancellation() }
        }
        val waiting=launch { task.use { fail("cancelled auth entered task") } }
        entered.await();assertEquals(2,owners());withTimeout(limit()) { waiting.cancelAndJoin() }
        assertEquals(0,owners())
    } }

    @Test fun lateSuccessfulAuthenticationCannotStartCancelledTask()=directory { root -> runBlocking {
        val entered=CompletableDeferred<Unit>();val release=CompletableDeferred<Unit>();var ran=false
        val task=task(root,FileEndpoint.APP,endpoint()) { _,_,session ->
            supply(session);entered.complete(Unit);withContext(NonCancellable) { release.await() }
        }
        val waiting=launch { task.use { ran=true } }
        entered.await();waiting.cancel();release.complete(Unit);waiting.join()
        assertFalse("late authentication must not enter copy execution",ran)
        assertTrue(waiting.isCancelled);assertEquals(0,owners())
    } }

    @Test fun childWorkFinishesBeforeScopeClosesAndFailureCleansAllOwners()=directory { root -> runBlocking {
        val source=endpoint();val destination=endpoint();var childCompleted=false
        task(root,source,destination).use { scope ->
            launch {
                delay(40)
                assertTrue(checkNotNull(scope.remote(Role.SOURCE,source)).session.browse(fixture.getString("remoteRoot")).path.startsWith('/'))
                childCompleted=true
            }
        }
        assertTrue(childCompleted);assertEquals(0,owners())
        val sentinel=IllegalArgumentException("child failure")
        val result=runCatching { task(root,source,destination).use { launch { throw sentinel };awaitCancellation() } }
        assertSame(sentinel,result.exceptionOrNull());assertEquals(0,owners())
    } }

    @Test fun operationFailureClosesOwnerAndTaskDoesNotRetry()=directory { root -> runBlocking {
        val source=endpoint();var calls=0
        val task=FileTaskConnections(source,FileEndpoint.APP,{ _,_->calls++;connection(known(root)) },{ _,_,s->supply(s) })
        task.use { scope ->
            val files=checkNotNull(scope.remote(Role.SOURCE,source)).session
            rejected { files.browse(fixture.getString("remoteRoot")+"/missing-${UUID.randomUUID()}") }
            rejected { scope.remote(Role.SOURCE,source) }
        }
        assertEquals(1,calls);assertEquals(0,owners())
    } }
}
