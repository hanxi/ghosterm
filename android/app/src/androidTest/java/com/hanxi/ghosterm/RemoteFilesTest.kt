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
class RemoteFilesTest {
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

    private suspend fun publishFile(files: RemoteFileSession, source: String, target: String,
                                    size: Long=4, directory: Boolean=false, budget: Int=5000): RemotePublicationResult =
        try { files.publish(source,target,directory,size,budget) }
        catch(cancelled: CancellationException) { throw cancelled }
        catch(failure: Exception) { throw AssertionError("publication outcome must reach caller",failure) }

    @Test fun conflictPlanCopiesRealRemoteBytesToNewLocalName() = directory { root ->
        val home=File(root,"home").apply { mkdir() };File(home,"文本.txt").writeText("local original")
        val local=LocalFileBrowser(home,instrumentation.targetContext.contentResolver)
        RemoteFileSession(connection(known(root))).use { files ->
            supply(files)
            FileBrowserRepository(local).use { repository -> runBlocking {
                val endpoint=repository.attach(RemoteFileIdentity("copy",host,fixture.getString("user"),fixture.getInt("port")),files)
                val remote=repository.browse(FileLocation(endpoint));val selected=remote.entries.single { it.name=="文本.txt" }
                val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(remote.location,"Remote",listOf(selected)),FileLocation(FileEndpoint.APP),"Local")
                val access=FileDestinationAccess(home,instrumentation.targetContext.contentResolver,repository::browse)
                val checked=FilePastePreflight(access::inspect,access::relation).check(FileTreeScanner(repository::browse).scan(request))
                assertTrue(checked.failure.toString(),checked.checked)
                val decisions=FilePasteDecisions.begin(checked)
                decisions.choose(decisions.state().reference,selected.name,FilePasteChoice.KEEP_BOTH)
                val plan=decisions.confirm(decisions.state().reference)
                assertEquals("文本 (2).txt",plan.roots.single().destinationName)
                assertEquals(selected.id,plan.entries.single().source.entry.id)
                val publisher=com.hanxi.ghosterm.data.LocalFilePublisher(home,File(root,"publications"))
                TransferStagingStore(File(root,"snapshots"),4096,8192,0).use { store ->
                    store.receive(files,selected.id,selected.size!!.toLong()).use { snapshot ->
                        assertEquals(com.hanxi.ghosterm.data.LocalFilePublisher.State.PUBLISHED,
                            publisher.publish("",plan.roots.single().destinationName,snapshot).state)
                    }
                    assertEquals(0L,store.reservedBytes)
                }
                assertEquals("远端 UTF-8\nline two\n",File(home,"文本 (2).txt").readText())
                assertEquals("local original",File(home,"文本.txt").readText())
                assertEquals("远端 UTF-8\nline two\n",files.previewText(selected.id));assertTrue(files.state().ready)
            } }
        }
    }

    @Test fun remoteTargetPreflightRetainsUnknownWritePermissionAndCanonicalConflicts() = directory { root ->
        val home=File(root,"home").apply { mkdir() };File(home,"文本.txt").writeText("local")
        val local=LocalFileBrowser(home,instrumentation.targetContext.contentResolver)
        RemoteFileSession(connection(known(root))).use { files ->
            supply(files)
            FileBrowserRepository(local).use { repository -> runBlocking {
                val endpoint=repository.attach(RemoteFileIdentity("check",host,fixture.getString("user"),fixture.getInt("port")),files)
                val remote=repository.browse(FileLocation(endpoint))
                val access=FileDestinationAccess(home,instrumentation.targetContext.contentResolver,repository::browse)
                val service=FilePastePreflight(access::inspect,access::relation)
                suspend fun check(source:FileLocation,target:FileLocation,items:List<BrowserEntry>):FilePasteCheck {
                    val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"Source",items),target,"Target")
                    return service.check(FileTreeScanner(repository::browse).scan(request))
                }
                val app=FileLocation(FileEndpoint.APP)
                val checked=check(app,remote.location,local.list(app))
                assertTrue(checked.failure.toString(),checked.checked)
                assertEquals(FileCreateCapability.UNKNOWN,checked.create)
                assertEquals(remote.location.directoryId+"/文本.txt",checked.conflicts.single().destination.id)
                val text=remote.entries.filter { it.name=="文本.txt" }
                assertEquals(FileCreateCapability.ALLOWED,check(remote.location,app,text).create)
                assertEquals(FilePasteCheckFailureKind.SAME_DIRECTORY,check(remote.location,remote.location,text).failure?.kind)
                val nested=remote.location.copy(directoryId=remote.entries.single { it.name=="folder" }.id)
                val within=check(remote.location,nested,remote.entries.filter { it.name=="folder" })
                assertEquals(FilePasteCheckFailureKind.INSIDE_SOURCE,within.failure?.kind)
                assertTrue(check(remote.location,nested,text).checked)
                assertEquals("远端 UTF-8\nline two\n",files.previewText(remote.location.directoryId+"/文本.txt"))
                assertEquals("local",File(home,"文本.txt").readText());assertTrue(files.state().ready)
                repository.disconnect(endpoint)
                val disconnected=service.check(checked.scan)
                assertEquals(FilePasteCheckFailureKind.TARGET_READ,disconnected.failure?.kind)
            } }
        }
    }

    @Test fun realRemoteTreeScanPreservesCanonicalOriginEmptyDirectoryAndLink() = directory { root ->
        val local=LocalFileBrowser(root,instrumentation.targetContext.contentResolver)
        RemoteFileSession(connection(known(root))).use { files ->
            supply(files)
            FileBrowserRepository(local).use { source -> runBlocking {
                val endpoint=source.attach(RemoteFileIdentity("scan",host,fixture.getString("user"),fixture.getInt("port")),files)
                val directory=source.browse(FileLocation(endpoint))
                val chosen=directory.entries.filter { it.name in setOf("文本.txt","zero","folder","link") }
                val intent=FilePasteRequest.capture(FileClipboardSnapshot.capture(directory.location,directory.location.directoryId,chosen),
                    FileLocation(FileEndpoint.APP),"App")
                val scan=FileTreeScanner(source::browse).scan(intent)
                assertTrue(scan.failure.toString(),scan.complete)
                assertEquals(setOf("文本.txt","zero","folder","link"),scan.entries.map { it.relativePath }.toSet())
                assertEquals(0,scan.entries.single { it.relativePath=="folder" }.childCount)
                assertEquals(2,scan.files);assertEquals(1,scan.directories);assertEquals(1,scan.unsupported)
                assertEquals("远端 UTF-8\nline two\n".toByteArray().size.toULong(),scan.knownBytes);assertNull(scan.totalBytes)
                assertEquals(fixture.getString("remoteRoot"),scan.entries.single { it.relativePath=="文本.txt" }.parent.directoryId)
                assertEquals(FileKind.LINK,scan.entries.single { it.relativePath=="link" }.entry.kind)
                assertTrue(files.state().ready)
                assertEquals("远端 UTF-8\nline two\n",files.previewText(fixture.getString("remoteRoot")+"/文本.txt"))
            } }
        }
    }
    @Test fun publishFilesAndConflictsPreserveOriginalBytes() = directory { root ->
        for(size in listOf(0,70001)) {
            val path=fixture.getString("remoteRoot")+"-publish-"+UUID.randomUUID()
            val content=ByteArray(size) { (it*31+7).toByte() }
            val source=File(root,"publish-source").apply { writeBytes(content) }
            android.os.ParcelFileDescriptor.open(source,android.os.ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
                RemoteFileSession(connection(known(root))).use { files ->
                    supply(files)
                    runBlocking {
                        assertEquals(RemoteCreationState.COMPLETE,createFile(files,path,fd.fd,size.toLong()).state)
                        val result=publishFile(files,path,path+"-final",size.toLong())
                        assertEquals(0,result.status);assertEquals(RemotePublicationState.PUBLISHED,result.state);assertEquals(0u,result.remoteStatus)
                        assertArrayEquals(content,files.preview(path+"-final"))
                        assertFalse(files.browse(path.substringBeforeLast('/')).entries.any { it.path==path })
                        assertEquals(RemoteCreationState.COMPLETE,createFile(files,path,fd.fd,size.toLong()).state)
                        val conflict=publishFile(files,path,path+"-final",size.toLong())
                        assertEquals(3,conflict.status);assertEquals(RemotePublicationState.CONFLICT,conflict.state);assertEquals(0u,conflict.remoteStatus)
                        assertTrue(files.state().finished)
                    }
                }
                RemoteFileSession(connection(known(root))).use { files ->
                    supply(files);runBlocking { assertArrayEquals(content,files.preview(path));assertArrayEquals(content,files.preview(path+"-final")) }
                }
            }
        }
    }
    @Test fun publishDirectoryPreservesEmptyDirectory() = directory { root ->
        val source=fixture.getString("remoteRoot")+"/folder";val target=fixture.getString("remoteRoot")+"-directory-"+UUID.randomUUID()
        RemoteFileSession(connection(known(root))).use { files ->
            supply(files)
            runBlocking {
                val result=publishFile(files,source,target,0,true)
                assertEquals(RemotePublicationState.PUBLISHED,result.state);assertEquals(0,result.status)
                try { assertTrue(files.browse(target).entries.isEmpty()) }
                finally { assertEquals(RemotePublicationState.PUBLISHED,publishFile(files,target,source,0,true).state) }
            }
        }
    }
    @Test fun publishProtocolPreservesRejectedUnknownAndQueryFailure() = directory { root ->
        for(stage in listOf("publish-target","publish-rename")) for(action in listOf("deny","publish-unknown","publish-connection-lost","publish-unsupported","publish-race","publish-malformed","publish-conflict")) {
            if(action=="publish-conflict" && stage!="publish-target") continue
            val user=control("new",stage=stage,action=action).getString("user")
            try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
                supply(files,true)
                val result=runBlocking { publishFile(files,"/item-source","/item-target") }
                val state=if(action=="publish-conflict") RemotePublicationState.CONFLICT
                    else if(stage!="publish-rename") RemotePublicationState.NOT_ATTEMPTED
                    else if(action in listOf("deny","publish-unsupported","publish-race")) RemotePublicationState.REJECTED
                    else RemotePublicationState.UNKNOWN
                val remote=when(action) { "deny"->3u;"publish-unknown"->0xdeadbeefu;"publish-connection-lost"->7u;"publish-unsupported"->8u;"publish-race"->4u;else->0u }
                assertEquals(state,result.state);assertEquals(remote,result.remoteStatus)
                assertEquals(if(stage=="publish-rename" && action=="publish-unsupported") 7 else 3,result.status)
                assertTrue(files.state().finished);assertTrue(reached(user,stage))
                if(stage!="publish-rename") assertFalse(reached(user,"publish-rename"))
            } } finally { control("release",user) }
        }
    }
    @Test fun publishTimeoutAndCancellationReleaseOwner() = directory { root ->
        val bound=limit()
        for(stage in listOf("publish-source","publish-target","publish-recheck","publish-rename")) {
            for(cancel in listOf(false,true)) {
                val user=control("new",stage=stage,action="hold").getString("user")
                try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
                    supply(files,true)
                    runBlocking {
                        val began=SystemClock.uptimeMillis()
                        if(cancel) {
                            val job=launch(Dispatchers.IO) { publishFile(files,"/item-source","/item-target",budget=10000) }
                            await { reached(user,stage) };job.cancelAndJoin()
                            assertTrue(job.isCancelled)
                        } else {
                            val result=publishFile(files,"/item-source","/item-target",budget=500)
                            assertEquals(3,result.status);assertEquals(if(stage=="publish-rename") RemotePublicationState.UNKNOWN else RemotePublicationState.NOT_ATTEMPTED,result.state)
                            assertTrue(SystemClock.uptimeMillis()-began>=498)
                        }
                        assertTrue(SystemClock.uptimeMillis()-began<bound)
                        assertTrue(files.state().finished)
                    }
                } } finally { control("release",user) }
                await { owners()==0 }
            }
        }
    }
    @Test fun publishInvalidArgumentsDoNotStartRemoteRequests() = directory { root ->
        RemoteFileSession(connection(known(root))).use { files ->
            supply(files)
            runBlocking {
                for(target in listOf("", "bad\u0000name", "bad\uD800name")) {
                    val failure=runCatching { files.publish("/source",target,expectedSize=4) }.exceptionOrNull()
                    assertTrue(failure is IllegalArgumentException || failure is CharacterCodingException)
                    assertTrue(files.state().ready)
                }
                assertTrue(runCatching { files.publish("/source","/target",expectedSize=4,timeoutMillis=0) }.exceptionOrNull() is IllegalArgumentException)
                assertTrue(runCatching { files.publish("/source","/target",true,4) }.exceptionOrNull() is IllegalArgumentException)
                assertTrue(files.state().ready)
            }
        }
    }
    @Test fun publishMissingSourcePreservesServerStatus() = directory { root ->
        RemoteFileSession(connection(known(root))).use { files ->
            supply(files)
            val path=fixture.getString("remoteRoot")+"-missing-"+UUID.randomUUID()
            val result=runBlocking { publishFile(files,path,path+"-final") }
            assertEquals(3,result.status);assertEquals(RemotePublicationState.NOT_ATTEMPTED,result.state);assertEquals(2u,result.remoteStatus)
            assertTrue(files.state().finished)
        }
    }

    @Test fun realDirectoryAndPreviewPreserveMetadataAndBytes() = directory { root ->
        RemoteFileSession(connection(known(root))).use { files ->
            supply(files)
            runBlocking {
                val path=fixture.getString("remoteRoot")
                val listing=files.browse(path)
                assertEquals(path,listing.path);assertEquals(7,listing.entries.size)
                val text=listing.entries.single { it.name=="文本.txt" }
                assertTrue(text.regularFile);assertFalse(text.directory);assertFalse(text.symbolicLink)
                assertEquals("远端 UTF-8\nline two\n".toByteArray().size.toULong(),text.size)
                assertEquals("远端 UTF-8\nline two\n",files.previewText(text.path))
                assertTrue(listing.entries.single { it.name=="folder" }.directory)
                assertTrue(listing.entries.single { it.name=="link" }.symbolicLink)
                assertEquals(0uL,listing.entries.single { it.name=="zero" }.size)
                assertEquals(0,files.preview(path+"/zero").size)
                assertArrayEquals("hello\u0000world".toByteArray(),files.preview(path+"/binary"))
                assertTrue(runCatching { files.previewText(path+"/binary") }.exceptionOrNull() is IllegalStateException)
                assertTrue(runCatching { files.previewText(path+"/invalid") }.exceptionOrNull() is CharacterCodingException)
                assertEquals(listing,files.browse(path))
            }
        }
    }
    @Test fun oversizedAndLinkedPreviewFailWithoutPublishing() = directory { root ->
        for((name,status) in listOf("large" to 3,"link" to 7)) {
            RemoteFileSession(connection(known(root))).use { files ->
                supply(files)
                val failure=runBlocking { runCatching { files.preview(fixture.getString("remoteRoot")+"/"+name) }.exceptionOrNull() }
                assertTrue("preview must fail: $name",failure is RemoteFileException)
                assertEquals(status,(failure as RemoteFileException).status)
                assertTrue(files.state().finished)
            }
            await { owners()==0 }
        }
    }
    @Test fun unknownHostTrustAndRejectionStayExplicit() = directory { root ->
        val trustFile=File(root,"new-known")
        RemoteFileSession(connection(trustFile)).use { files ->
            val prompt=phase(files,SSHAuthPhase.HOSTKEY)
            assertEquals(SSHHostState.UNKNOWN,prompt.host);assertEquals(fixture.getString("fingerprint"),prompt.fingerprint)
            files.decide(prompt,false)
            assertEquals(5,phase(files,SSHAuthPhase.FAILED).error);assertFalse(trustFile.exists())
        }
        RemoteFileSession(connection(trustFile)).use { files ->
            files.decide(phase(files,SSHAuthPhase.HOSTKEY),true);supply(files)
            assertTrue(trustFile.readText().contains("[$host]:${fixture.getInt("port")}"))
        }
    }
    @Test fun authenticationScopesAndStalePromptsCannotCrossConnections() = directory { root ->
        val known=known(root)
        RemoteFileSession(connection(known)).use { a -> RemoteFileSession(connection(known)).use { b ->
            RemoteSession(connection(known)).use { terminal ->
                val first=phase(a,SSHAuthPhase.CREDENTIALS);val other=phase(b,SSHAuthPhase.CREDENTIALS)
                val owner=SSHAuthState::class.java.getDeclaredField("owner").apply { isAccessible=true }
                val old=owner.getLong(first)
                // Force the numeric-ID collision of separate native registries; scope must still reject.
                owner.setLong(first,owner.getLong(other))
                val secret=fixture.getString("filePem").toByteArray()
                assertThrows(IllegalStateException::class.java) { b.privateKey(first,secret) }
                assertTrue(secret.all { it==0.toByte() })
                await { terminal.authentication().phase==SSHAuthPhase.CREDENTIALS }
                owner.setLong(first,owner.getLong(terminal.authentication()))
                val terminalSecret=fixture.getString("pem").toByteArray()
                assertThrows(IllegalStateException::class.java) { terminal.privateKey(first,terminalSecret) }
                assertTrue(terminalSecret.all { it==0.toByte() })
                owner.setLong(first,old);supply(a);supply(b)
                val stale=fixture.getString("filePem").toByteArray()
                assertThrows(IllegalStateException::class.java) { a.privateKey(first,stale) }
                assertTrue(stale.all { it==0.toByte() })
            }
        } }
    }
    @Test fun closingFilesKeepsOriginalTerminalPidAndIo() = directory { root ->
        val known=known(root)
        RemoteSession(connection(known)).use { terminal ->
            await { terminal.authentication().phase==SSHAuthPhase.CREDENTIALS }
            terminal.privateKey(terminal.authentication(),fixture.getString("pem").toByteArray())
            await { terminal.state().connected }
            fun pid(): String {
                terminal.commit("stty -echo; PS1=''; printf '\\033[2J\\033[HFILE_PID_%s\\n' \"$$\"\r")
                var value: String?=null
                await { terminal.frame(0)?.let { frame ->
                    value=(0 until frame.rows).map(frame::line).firstOrNull { it.trim().matches(Regex("FILE_PID_[0-9]+")) }
                };value!=null }
                return value!!
            }
            val before=pid()
            RemoteFileSession(connection(known)).use { files ->
                supply(files);runBlocking { files.browse(fixture.getString("remoteRoot")) }
                assertEquals(before,pid())
            }
            await { owners()==0 }
            terminal.commit("printf '\\033[2J\\033[HAFTER_FILES_CLOSE\\n'\r")
            await { terminal.frame(0)?.line(0)?.trim()=="AFTER_FILES_CLOSE" }
            assertEquals(before,pid());assertTrue(terminal.state().connected)
        }
    }
    private fun cancelledRequest(preview: Boolean) = directory { root ->
        val stage=if(preview) "preview-read" else "readdir"
        val user=control("new",stage=stage,action="hold").getString("user")
        val allowance=limit()
        try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
            supply(files,true)
            runBlocking {
                val job=launch(Dispatchers.IO) { if(preview) files.preview("/file") else files.browse(".") }
                try {
                    await { reached(user,stage) };job.cancelAndJoin()
                    await(allowance,"cancelled file request must release owner") { owners()==0 }
                    assertTrue(files.state().finished)
                } finally { job.cancelAndJoin() }
            }
        } } finally { control("release",user) }
    }
    @Test fun directoryCancellationReleasesOwner() = cancelledRequest(false)
    @Test fun previewCancellationReleasesOwner() = cancelledRequest(true)
    @Test fun directoryRequestUsesItsOwnBudget() = directory { root ->
        val user=control("new",stage="readdir",action="hold").getString("user")
        try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
            supply(files,true)
            val start=SystemClock.uptimeMillis()
            val failure=runBlocking { runCatching { withTimeout(2000) { files.browse(".",300) } }.exceptionOrNull() }
            assertTrue("directory budget must report IO",failure is RemoteFileException)
            assertEquals(3,(failure as RemoteFileException).status)
            assertTrue(SystemClock.uptimeMillis()-start>=290)
        } } finally { control("release",user) }
    }
    @Test fun idleCloseAbortsBeforeWaitingForPeer() = directory { root ->
        val user=control("new",stage="realpath").getString("user");val allowance=limit()
        try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
            supply(files,true);runBlocking { files.browse(".") };control("freeze",user)
            val start=SystemClock.uptimeMillis()
            instrumentation.runOnMainSync { files.close() }
            assertTrue("file close blocks main thread",SystemClock.uptimeMillis()-start<allowance)
            await(allowance,"idle file owner must abort before cleanup") { owners()==0 }
        } } finally { control("release",user) }
    }
    @Test fun requestIdsAndRecycledHandlesRejectStaleCalls() = directory { root ->
        val files=RemoteFileSession(connection(known(root)));supply(files)
        val field=RemoteFileSession::class.java.getDeclaredField("handle").apply { isAccessible=true }
        val id=field.getLong(files)
        try {
            val request=native("begin",id,1,fixture.getString("remoteRoot").toByteArray(),5000) as Long
            await { native("result",id,request)!=null }
            assertThrows(IllegalStateException::class.java) { native("result",id,request+1) }
            native("finish",id,request)
            assertThrows(IllegalStateException::class.java) { native("result",id,request) }
        } finally { files.close() }
        RemoteFileSession(connection(known(root))).use { next ->
            supply(next)
            assertThrows(IllegalStateException::class.java) { native("state",id) }
            assertEquals(7,runBlocking { next.browse(fixture.getString("remoteRoot")) }.entries.size)
        }
    }
    @Test fun unknownMetadataAndInvalidNamesRemainExplicit() = directory { root ->
        for(action in listOf("allow","invalid-utf8-name")) {
            val user=control("new",stage="readdir",action=action).getString("user")
            try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
                supply(files,true)
                if(action=="allow") {
                    val list=runBlocking { files.browse(".") }
                    val unknown=list.entries.single { it.name=="unknown" }
                    assertNull(unknown.size);assertFalse(unknown.regularFile);assertNull(unknown.modifiedSeconds)
                    val zero=list.entries.single { it.name=="zero" };assertEquals(0uL,zero.size);assertTrue(zero.regularFile)
                } else assertTrue(runBlocking { runCatching { files.browse(".") }.exceptionOrNull() } is CharacterCodingException)
            } } finally { control("release",user) }
        }
    }
    @Test fun malformedPathsAreRejectedWithoutBreakingConnection() = directory { root ->
        RemoteFileSession(connection(known(root))).use { files ->
            supply(files)
            runBlocking {
                assertTrue(runCatching { files.browse("bad\u0000path") }.exceptionOrNull() is IllegalArgumentException)
                assertTrue(runCatching { files.browse("bad\uD800path") }.exceptionOrNull() is CharacterCodingException)
                assertEquals(7,files.browse(fixture.getString("remoteRoot")).entries.size)
            }
        }
    }
    @Test fun cancellationDuringVersionNegotiationReleasesOwner() = directory { root ->
        val user=control("new",stage="version",action="hold").getString("user");val allowance=limit()
        try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
            supply(files,true,false);await { reached(user,"version") }
            assertFalse(files.state().ready);files.close()
            await(allowance,"file SFTP open cancellation releases owner") { owners()==0 }
        } } finally { control("release",user) }
    }
    private suspend fun createFile(files: RemoteFileSession, path: String, fd: Int, size: Long,
                                   budget: Int = 5000): RemoteCreationResult =
        try { files.createExclusive(path,fd,size,budget) }
        catch(cancelled: CancellationException) { throw cancelled }
        catch(failure: Exception) { throw AssertionError("creation outcome must reach caller",failure) }

    @Test fun exclusiveCreatePreservesContentOffsetAndExistingTarget() = directory { root ->
        val path=fixture.getString("remoteRoot")+"-created-"+UUID.randomUUID()
        val content=ByteArray(100003) { (it*31+7).toByte() }
        val source=File(root,"create-source").apply { writeBytes(content) }
        android.os.ParcelFileDescriptor.open(source,android.os.ParcelFileDescriptor.MODE_READ_ONLY).use { descriptor ->
            android.system.Os.lseek(descriptor.fileDescriptor,7,android.system.OsConstants.SEEK_SET)
            RemoteFileSession(connection(known(root))).use { files ->
                supply(files)
                val result=runBlocking { createFile(files,path,descriptor.fd,source.length()) }
                assertEquals(0,result.status);assertEquals(RemoteCreationState.COMPLETE,result.state)
                assertEquals(content.size.toULong(),result.bytesAcknowledged)
                assertEquals(7L,runCatching { android.system.Os.lseek(descriptor.fileDescriptor,0,android.system.OsConstants.SEEK_CUR) }.getOrDefault(-1L))
                assertArrayEquals(content,runBlocking { files.preview(path) })
                val denied=runBlocking { createFile(files,path,descriptor.fd,source.length()) }
                assertEquals(3,denied.status);assertEquals(RemoteCreationState.REJECTED,denied.state)
                assertEquals(0uL,denied.bytesAcknowledged);assertTrue(files.state().finished)
            }
            RemoteFileSession(connection(known(root))).use { files ->
                supply(files);assertArrayEquals(content,runBlocking { files.preview(path) })
            }
        }
    }
    @Test fun exclusiveCreateEmptyAndInvalidSourceAreDistinct() = directory { root ->
        val path=fixture.getString("remoteRoot")+"-created-empty-"+UUID.randomUUID()
        val source=File(root,"empty-source").apply { writeBytes(byteArrayOf()) }
        android.os.ParcelFileDescriptor.open(source,android.os.ParcelFileDescriptor.MODE_READ_ONLY).use { descriptor ->
            RemoteFileSession(connection(known(root))).use { files ->
                supply(files)
                val result=runBlocking { createFile(files,path,descriptor.fd,0) }
                assertEquals(RemoteCreationState.COMPLETE,result.state);assertEquals(0,result.status)
                assertEquals(0,runBlocking { files.preview(path) }.size)
                val invalid=runBlocking { createFile(files,path+"-bad",descriptor.fd,1) }
                assertEquals(1,invalid.status);assertEquals(RemoteCreationState.NOT_ATTEMPTED,invalid.state)
            }
            RemoteFileSession(connection(known(root))).use { files ->
                supply(files)
                assertFalse(runBlocking { files.browse(path.substringBeforeLast('/')) }.entries.any { it.path==path+"-bad" })
            }
        }
    }
    @Test fun exclusiveCreateProtocolOutcomesSurviveFailure() = directory { root ->
        val source=File(root,"source").apply { writeBytes(ByteArray(4096) { (it*31+7).toByte() }) }
        android.os.ParcelFileDescriptor.open(source,android.os.ParcelFileDescriptor.MODE_READ_ONLY).use { descriptor ->
            for(stage in listOf("create-open","create-write","create-close")) {
                for(action in listOf("deny","hold")) {
                    val user=control("new",stage=stage,action=action).getString("user")
                    try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
                        supply(files,true)
                        val result=runBlocking { createFile(files,"/created",descriptor.fd,source.length(),1000) }
                        assertEquals(3,result.status)
                        val expected=if(stage!="create-open") RemoteCreationState.CREATED
                            else if(action=="deny") RemoteCreationState.REJECTED else RemoteCreationState.UNKNOWN
                        assertEquals(expected,result.state);assertTrue(files.state().finished)
                        assertTrue(reached(user,stage))
                    } } finally { control("release",user) }
                }
            }
        }
    }
    @Test fun exclusiveCreateCancellationReleasesSourceDuplicate() = directory { root ->
        val source=File(root,"source").apply { writeBytes(ByteArray(4096) { (it*31+7).toByte() }) }
        fun sourceHandles()=File("/proc/self/fd").listFiles().orEmpty().filter {
            runCatching { File(android.system.Os.readlink(it.absolutePath)).canonicalPath==source.canonicalPath }.getOrDefault(false)
        }
        fun sourceDescriptors()=sourceHandles().size
        val bound=limit()
        val user=control("new",stage="create-write",action="hold").getString("user")
        try {
            android.os.ParcelFileDescriptor.open(source,android.os.ParcelFileDescriptor.MODE_READ_ONLY).use { descriptor ->
                RemoteFileSession(connection(known(root,true),true,user)).use { files ->
                    supply(files,true)
                    runBlocking {
                        val job=launch(Dispatchers.IO) { createFile(files,"/cancelled",descriptor.fd,source.length()) }
                        await { reached(user,"create-write") };assertEquals(2,sourceDescriptors())
                        sourceHandles().forEach { handle ->
                            val flags=File("/proc/self/fdinfo/"+handle.name).readLines().single { it.startsWith("flags:") }.substringAfter(':').trim().toLong(8)
                            assertTrue("source descriptors must be CLOEXEC",flags and 0x80000L != 0L)
                        }
                        val cancelStarted=SystemClock.uptimeMillis()
                        job.cancelAndJoin()
                        assertTrue("create cancellation must not wait for the request timeout",SystemClock.uptimeMillis()-cancelStarted<bound)
                        assertEquals("create returned before source duplicate closed",1,sourceDescriptors())
                    }
                    await(timeout=bound,message="cancelled create owner must release within bound") { owners()==0 };assertEquals(1,sourceDescriptors())
                }
            }
            assertEquals(0,sourceDescriptors())
        } finally { control("release",user) }
    }

    @Test fun stagedLocalSourceUploadsFrozenBytesWithoutExposingPrivatePaths() = directory { root ->
        val home=File(root,"home").apply { mkdir() }
        val source=File(home,"snapshot.txt").apply { writeText("frozen 中文 content\n") }
        val local=LocalFileBrowser(home,instrumentation.targetContext.contentResolver)
        TransferStagingStore(File(root,"staging"),1024,2048,0).use { store ->
            RemoteFileSession(connection(known(root))).use { files ->
                supply(files)
                runBlocking {
                    val location=FileLocation(FileEndpoint.APP)
                    val selected=local.list(location).single()
                    local.snapshot(location,selected,store).use { snapshot ->
                        source.writeText("changed after copy")
                        val path=fixture.getString("remoteRoot")+"-stage-"+UUID.randomUUID()
                        val created=snapshot.withDescriptor { fd,size -> files.createExclusive(path,fd,size) }
                        assertEquals(0,created.status)
                        assertEquals(RemoteCreationState.COMPLETE,created.state)
                        assertEquals("frozen 中文 content\n",files.previewText(path))
                        assertEquals(listOf("snapshot.txt"),local.list(location).map { it.name })
                        assertEquals("changed after copy",source.readText())
                    }
                    assertEquals(0L,store.reservedBytes)
                }
            }
        }
    }

    @Test fun receiveRegularFilePreservesOffsetAndOwnedDescriptor() = directory { root ->
        val path=fixture.getString("remoteRoot")+"-receive-"+UUID.randomUUID()
        val bytes=ByteArray(70001) { (it*31+7).toByte() }
        val source=File(root,"upload").apply { writeBytes(bytes) }
        android.os.ParcelFileDescriptor.open(source,android.os.ParcelFileDescriptor.MODE_READ_ONLY).use { input ->
            RemoteFileSession(connection(known(root))).use { files ->
                supply(files);assertEquals(0,runBlocking { files.createExclusive(path,input.fd,bytes.size.toLong()) }.status)
                val target=File(root,"target").apply { writeBytes(byteArrayOf()) }
                android.os.ParcelFileDescriptor.open(target,android.os.ParcelFileDescriptor.MODE_READ_WRITE).use { output ->
                    android.system.Os.lseek(output.fileDescriptor,7,android.system.OsConstants.SEEK_SET)
                    assertEquals(bytes.size.toLong(),runBlocking { files.receive(path,output.fd,bytes.size.toLong()) })
                    assertEquals(7L,android.system.Os.lseek(output.fileDescriptor,0,android.system.OsConstants.SEEK_CUR))
                    assertArrayEquals(bytes,target.readBytes());assertTrue(files.state().ready)
                }
            }
        }
    }
    @Test fun receiveAnonymousSnapshotsSealBytesAndRetainBudget() = directory { root ->
        val path=fixture.getString("remoteRoot")+"-snapshot-receive-"+UUID.randomUUID()
        for(size in listOf(0,70001)) {
            val bytes=ByteArray(size) { (it*31+7).toByte() }
            val source=File(root,"source-$size").apply { writeBytes(bytes) }
            android.os.ParcelFileDescriptor.open(source,android.os.ParcelFileDescriptor.MODE_READ_ONLY).use { input ->
                RemoteFileSession(connection(known(root))).use { files ->
                    supply(files);assertEquals(0,runBlocking { files.createExclusive(path+size,input.fd,size.toLong()) }.status)
                    for(expected in listOf(size.toLong(),null)) {
                        val dir=File(root,"private-$size-$expected")
                        TransferStagingStore(dir,80000,80000,0).use { store ->
                            runBlocking {
                                store.receive(files,path+size,expected).use { snapshot ->
                                    assertEquals(size.toLong(),snapshot.size)
                                    assertEquals(expected ?: 80000L,store.reservedBytes)
                                    assertEquals(0,dir.listFiles().orEmpty().size)
                                    snapshot.withDescriptor { fd,count ->
                                        val descriptor=android.os.ParcelFileDescriptor.fromFd(fd)
                                        assertEquals(0,android.system.Os.fcntlInt(descriptor.fileDescriptor,android.system.OsConstants.F_GETFL,0) and android.system.OsConstants.O_ACCMODE)
                                        val got=android.os.ParcelFileDescriptor.AutoCloseInputStream(descriptor).use { it.readBytes() }
                                        assertEquals(size.toLong(),count);assertArrayEquals(bytes,got)
                                    }
                                }
                            }
                            assertEquals(0L,store.reservedBytes)
                        }
                    }
                }
            }
        }
    }
    @Test fun receiveProtocolFailuresNeverPublishSnapshot() = directory { root ->
        val stages=listOf("receive-lstat","receive-open","receive-fstat","receive-read","receive-finalstat","receive-close")
        val cases=stages.flatMap { listOf(it to "deny",it to "hold") } + listOf(
            "receive-read" to "receive-shrink", "receive-read" to "receive-overflow",
            "receive-finalstat" to "receive-changed-mtime", "receive-finalstat" to "receive-missing-size")
        for((stage,action) in cases) {
            val user=control("new",stage=stage,action=action).getString("user")
            try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
                supply(files,true)
                TransferStagingStore(File(root,"failed"),300,300,0).use { store ->
                    val started=SystemClock.uptimeMillis()
                    val failed=runCatching { runBlocking { store.receive(files,"/file",timeoutMillis=500) } }.exceptionOrNull()
                    if(action=="hold") assertTrue("receive must use its requested budget",SystemClock.uptimeMillis()-started>=498)
                    assertTrue("receive failure must remain explicit: $failed",failed is RemoteFileException)
                    await { store.reservedBytes==0L };assertTrue(files.state().finished)
                    assertTrue(reached(user,stage));assertEquals(0,File(root,"failed").listFiles().orEmpty().size)
                }
            } } finally { control("release",user) }
        }
    }
    @Test fun receiveCancellationWaitsForNativeDescriptorRelease() = directory { root ->
        for(stage in listOf("receive-lstat","receive-open","receive-fstat","receive-read","receive-finalstat","receive-close")) {
            val user=control("new",stage=stage,action="hold").getString("user")
            try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
                supply(files,true)
                val target=File(root,"receive-cancel").apply { writeBytes(byteArrayOf()) }
                fun handles()=File("/proc/self/fd").listFiles().orEmpty().count {
                    runCatching { File(android.system.Os.readlink(it.path)).canonicalPath==target.canonicalPath }.getOrDefault(false)
                }
                android.os.ParcelFileDescriptor.open(target,android.os.ParcelFileDescriptor.MODE_READ_WRITE).use { descriptor ->
                    runBlocking {
                        val request=launch(Dispatchers.IO) { files.receive("/file",descriptor.fd,300) }
                        await { reached(user,stage) };assertEquals(2,handles())
                        File("/proc/self/fd").listFiles().orEmpty().filter {
                            runCatching { File(android.system.Os.readlink(it.path)).canonicalPath==target.canonicalPath }.getOrDefault(false)
                        }.forEach {
                            val flags=File("/proc/self/fdinfo/"+it.name).readLines().single { line -> line.startsWith("flags:") }.substringAfter(':').trim().toLong(8)
                            assertTrue("receive descriptors must be CLOEXEC",flags and 0x80000L != 0L)
                        }
                        request.cancelAndJoin()
                        assertEquals("cancel returned while native still holds the target",1,handles())
                    }
                }
                assertEquals(0,handles())
            } } finally { control("release",user) }
        }
    }
    @Test fun storeCloseCancelsPendingReceiveAndReleasesReservation() = directory { root ->
        val user=control("new",stage="receive-read",action="hold").getString("user")
        try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
            supply(files,true)
            val store=TransferStagingStore(File(root,"close-pending"),300,300,0)
            runBlocking {
                val request=async(Dispatchers.IO) { runCatching { store.receive(files,"/file") } }
                await { reached(user,"receive-read") };assertEquals(300L,store.reservedBytes)
                val bound=limit();val started=SystemClock.uptimeMillis()
                store.close()
                assertTrue(request.await().isFailure)
                assertTrue("store close must cancel before request timeout",SystemClock.uptimeMillis()-started<bound)
                await { store.reservedBytes==0L }
                assertEquals(0,File(root,"close-pending").listFiles().orEmpty().size)
            }
        } } finally { control("release",user) }
    }
    @Test fun cancelledSnapshotUploadKeepsLeaseUntilNativeRelease() = directory { root ->
        val source=File(root,"lease-source").apply { writeBytes(ByteArray(4096) { (it*31+7).toByte() }) }
        val user=control("new",stage="create-write",action="hold").getString("user")
        try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
            supply(files,true)
            TransferStagingStore(File(root,"lease"),4096,4096,0).use { store ->
                runBlocking {
                    val snapshot=store.snapshot(4096) {
                        android.content.res.AssetFileDescriptor(android.os.ParcelFileDescriptor.open(source,android.os.ParcelFileDescriptor.MODE_READ_ONLY),0,4096)
                    }
                    var inode=0L
                    val request=launch(Dispatchers.IO) {
                        snapshot.withDescriptor { fd,size ->
                            android.os.ParcelFileDescriptor.fromFd(fd).use { inode=android.system.Os.fstat(it.fileDescriptor).st_ino }
                            files.createExclusive("/cancelled",fd,size)
                        }
                    }
                    await { reached(user,"create-write") };snapshot.close()
                    assertEquals(4096L,store.reservedBytes)
                    request.cancelAndJoin()
                    assertEquals(0L,store.reservedBytes)
                    val remaining=File("/proc/self/fd").listFiles().orEmpty().count {
                        runCatching { android.system.Os.stat(it.path).st_ino==inode }.getOrDefault(false)
                    }
                    assertEquals("snapshot capacity released while native still owns its inode",0,remaining)
                }
            }
        } } finally { control("release",user) }
    }

    @Test fun receiveDeliveryCancellationClosesUnclaimedSnapshot() = directory { root ->
        val user=control("new",stage="receive-read",action="allow").getString("user")
        try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
            supply(files,true)
            TransferStagingStore(File(root,"delivery"),300,300,0).use { store ->
                runBlocking {
                    val queue=java.util.concurrent.LinkedBlockingQueue<Runnable>()
                    val dispatcher=object:CoroutineDispatcher() {
                        override fun dispatch(context:kotlin.coroutines.CoroutineContext,block:Runnable) { queue.add(block) }
                    }
                    val request=launch(dispatcher,start=CoroutineStart.UNDISPATCHED) {
                        store.receive(files,"/file").use { fail("cancelled delivery published remote snapshot") }
                    }
                    val delivery=withContext(Dispatchers.IO) { queue.poll(5,java.util.concurrent.TimeUnit.SECONDS) }
                    assertNotNull("remote snapshot must queue delivery",delivery)
                    assertEquals(300L,store.reservedBytes)
                    request.cancel();delivery!!.run();request.join()
                    assertEquals(0L,store.reservedBytes)
                }
            }
        } } finally { control("release",user) }
    }

    @Test fun receiveFrozenSizeMismatchDoesNotPublishSnapshot() = directory { root ->
        val user=control("new",stage="receive-read",action="allow").getString("user")
        try { RemoteFileSession(connection(known(root,true),true,user)).use { files ->
            supply(files,true)
            TransferStagingStore(File(root,"changed-size"),300,300,0).use { store ->
                val failure=runCatching { runBlocking { store.receive(files,"/file",258) } }.exceptionOrNull()
                assertTrue("frozen source length mismatch must fail",failure is IllegalStateException)
                await { store.reservedBytes==0L }
                assertTrue(files.state().ready)
            }
        } } finally { control("release",user) }
    }

}
