package com.hanxi.ghosterm

import android.os.SystemClock
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
}
