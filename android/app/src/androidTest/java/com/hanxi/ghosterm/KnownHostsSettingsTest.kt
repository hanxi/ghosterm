package com.hanxi.ghosterm

import android.app.Application
import android.os.Looper
import android.os.SystemClock
import android.system.Os
import android.view.View
import android.view.ViewGroup
import androidx.activity.compose.setContent
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.ViewModelStore
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.KnownHostsUpdate
import com.hanxi.ghosterm.bridge.SSHAuthPhase
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.terminal.TerminalView
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

@RunWith(AndroidJUnit4::class)
class KnownHostsSettingsTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val fixture by lazy { sshFixture() }
    private lateinit var model: KnownHostsViewModel
    private lateinit var hosts: HostsViewModel
    private lateinit var sessions: SessionsViewModel
    private lateinit var directory: File
    private lateinit var known: File
    private lateinit var config: File
    private var oldKnown: ByteArray? = null
    private var oldConfig: ByteArray? = null
    private var siblings = emptySet<String>()
    private val injected = ViewModelStore()
    private var identity: StoredIdentity? = null
    private val publicKey get() = fixture.getString("public").trim().split(Regex("\\s+")).take(2).joinToString(" ")
    private fun line(host: String = "fixture.example") = "$host $publicKey\n"
    private fun <T> main(block: () -> T): T {
        var result: T? = null
        instrumentation.runOnMainSync { result = block() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun await(label: String = "known_hosts state", predicate: () -> Boolean) {
        try { ui.waitUntil(15_000) { main(predicate) } }
        catch (_: ComposeTimeoutException) {
            fail(label + main { "; page=${hosts.page}, loading=${model.loading}, writing=${model.writing}, error=${model.error}, notice=${model.notice}" })
        }
    }
    @Before fun setup() {
        main {
            model = ViewModelProvider(ui.activity)[KnownHostsViewModel::class.java]
            hosts = ViewModelProvider(ui.activity)[HostsViewModel::class.java]
            sessions = ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
        }
        await { !hosts.busy }
        directory = File(ui.activity.filesDir, "home/.ssh"); directory.mkdirs()
        known = File(directory, "known_hosts"); config = File(directory, "config")
        oldKnown = known.takeIf(File::exists)?.readBytes(); oldConfig = config.takeIf(File::exists)?.readBytes()
        siblings = directory.list().orEmpty().toSet()
        known.delete()
    }
    @After fun cleanup() {
        if (::model.isInitialized) {
            main { model.leave(); sessions.connecting?.let { sessions.cancelConnection(it.id) }; sessions.sessions.map { it.id }.forEach(sessions::close) }
            await("write finished before fixture restoration") { !model.writing }
            main { injected.clear() }
        }
        identity?.let { IdentityVault(File(ui.activity.filesDir,"identities")).remove(it) }
        if (::known.isInitialized) {
            if (known.isDirectory) check(known.delete())
            oldKnown?.let(known::writeBytes) ?: known.delete()
            oldConfig?.let(config::writeBytes) ?: config.delete()
            directory.listFiles().orEmpty().filter { it.name !in siblings && it.name.startsWith(".ghosterm-") }.forEach { check(it.delete()) }
        }
    }
    private fun settings() {
        ui.onNodeWithText("设置").performClick()
        ui.onNodeWithText("主机验证记录").performScrollTo().performClick()
        await { hosts.page == Page.KNOWN_HOSTS && !model.busy && (model.snapshot != null || model.error != null) }
    }
    private fun open(index: Int = 0) { ui.onNodeWithTag("known-host-row-$index").performScrollTo().performClick() }
    private fun request() { ui.onNodeWithText("移除此记录").performScrollTo().performClick() }
    private fun confirm() { ui.onNodeWithText("确认移除记录").performClick(); await { !model.busy && model.selected == null } }
    private fun inject(repository: KnownHostsRepository) {
        main {
            model = KnownHostsViewModel(ui.activity.application as Application, repository)
            injected.put("known-hosts", model)
            ui.activity.setContent { GhostermApp(knownHosts = model) }
        }
        ui.waitForIdle()
    }
    @Test fun duplicateRemovalUsesExactRecordAndConfirmationSurvivesRecreation() {
        val row = line("first.example,*.example,!except.example").trimEnd()
        val original = "# 注释\r\n$row\r\n\r\n$row".toByteArray()
        known.writeBytes(original); settings()
        assertEquals(2, main { model.snapshot!!.records.map { it.id }.toSet().size })
        open(1); request()
        ui.activityRule.scenario.recreate()
        assertSame(model,main { ViewModelProvider(ui.activity)[KnownHostsViewModel::class.java] })
        ui.onNodeWithText("保留记录").performClick(); assertArrayEquals(original, known.readBytes())
        ui.onNodeWithTag("known-host-hosts").performScrollTo().assertTextEquals("first.example,*.example,!except.example")
        request(); confirm()
        assertArrayEquals("# 注释\r\n$row\r\n\r\n".toByteArray(),known.readBytes())
        ui.onNodeWithTag("known-hosts-notice").assertTextEquals("记录已移除")
        assertEquals(1,main { model.snapshot!!.records.size })
        ui.onNodeWithText("返回").performClick(); assertEquals(Page.SETTINGS,main { hosts.page })
    }
    @Test fun ruleKindsAndMalformedRecordsStayVisibleAndReadOnly() {
        known.writeText("@revoked ${line()}@cert-authority ${line()}@unknown ${line()}${line("|1|c2FsdA==|aGFzaA==")}broken row\n")
        settings()
        val kinds=listOf("已撤销的公钥 · 只读","证书机构规则 · 只读","不支持的规则 · 只读","哈希主机名 · 暂不支持匹配","未识别记录 · 只读")
        assertEquals(5,main { model.snapshot!!.records.size })
        for ((index,kind) in kinds.withIndex()) {
            open(index); ui.onNodeWithTag("known-host-kind").assertTextEquals(kind)
            ui.onNodeWithTag("known-host-readonly").performScrollTo().assertExists()
            ui.onNodeWithText("移除此记录").assertDoesNotExist()
            main { model.requestRemoval() }; assertFalse(main { model.confirmingRemoval })
            ui.onNodeWithText("返回").performClick(); assertEquals(Page.KNOWN_HOSTS,main { hosts.page })
        }
    }
    @Test fun publicFingerprintUsesWireBytesAndRejectsWrongAlgorithms() {
        val blob=publicKey.substringAfter(' ')
        known.writeText(line()+"bad.example ssh-ed25519 !!!!\nwrong.example ssh-rsa $blob\n"+line("hidden\u202E.example"))
        settings(); open()
        ui.onNodeWithTag("known-host-fingerprint").performScrollTo().assertTextEquals(fixture.getString("fingerprint"))
        ui.onNodeWithText("返回").performClick()
        for(index in 1..2) {
            open(index); ui.onNodeWithTag("known-host-fingerprint-unavailable").performScrollTo().assertExists()
            ui.onNodeWithTag("known-host-fingerprint").assertDoesNotExist()
            ui.onNodeWithText("返回").performClick()
        }
        open(3); ui.onNodeWithTag("known-host-hosts").assertTextEquals("hidden\\u202e.example")
    }
    @Test fun staleConfirmationPreservesExternalEditAndReloadsActualFile() {
        known.writeText(line()); settings(); open(); request()
        val changed=line("changed.example").toByteArray(); known.writeBytes(changed)
        confirm()
        assertArrayEquals(changed,known.readBytes())
        ui.onNodeWithTag("known-hosts-notice").assertTextContains("记录已变化",substring=true)
        assertEquals("changed.example",main { model.snapshot!!.records.single().record.hosts })
    }
    @Test fun readFailureIsNotEmptySuccessAndReloadRecovers() {
        check(known.mkdir()); settings()
        ui.onNodeWithTag("known-hosts-error").assertExists(); ui.onNodeWithTag("known-hosts-empty").assertDoesNotExist()
        assertNull(main { model.snapshot })
        check(known.delete()); known.writeText(line())
        ui.onNodeWithText("重新载入记录").performClick(); await { !model.busy && model.snapshot!=null }
        ui.onNodeWithTag("known-hosts-error").assertDoesNotExist(); open()
        ui.onNodeWithTag("known-host-fingerprint").performScrollTo().assertTextEquals(fixture.getString("fingerprint"))
        ui.onNodeWithText("返回").performClick(); known.delete()
        ui.onNodeWithText("重新载入记录").performClick(); await { !model.busy && model.snapshot?.records?.isEmpty()==true }
        ui.onNodeWithTag("known-hosts-empty").assertExists()
    }
    @Test fun repositoryRejectsLinksSpecialFilesAndInvalidBytes() {
        val root=File(ui.activity.cacheDir,"trust-${UUID.randomUUID()}"); check(root.mkdir())
        val target=File(root,"known_hosts"); val repo=KnownHostsRepository(root)
        try {
            assertTrue(repo.load().records.isEmpty())
            val original=line().toByteArray(); target.writeBytes(original)
            val copy=original.copyOf(); val snapshot=KnownHostsSnapshot(copy,"copied"); copy.fill(0)
            assertTrue(repo.remove(snapshot,snapshot.records.single().id).succeeded)
            assertEquals("",target.readText()); target.writeBytes(original)
            val outside=File(root,"outside"); target.renameTo(outside)
            Os.symlink(outside.absolutePath,target.absolutePath)
            assertThrows(Exception::class.java) { repo.load() }; check(target.delete())
            Os.mkfifo(target.absolutePath,384)
            assertThrows(Exception::class.java) { repo.load() }; check(target.delete())
            target.writeBytes(byteArrayOf(0xc3.toByte(),0x28))
            assertThrows(Exception::class.java) { repo.load() }
            target.writeBytes(ByteArray(1024*1024+1){32})
            assertThrows(Exception::class.java) { repo.load() }
            target.writeText("\u0000"); assertThrows(Exception::class.java) { repo.load() }
            target.writeBytes(original)
            val parentLink=File(root,"linked"); Os.symlink(root.absolutePath,parentLink.absolutePath)
            assertThrows(Exception::class.java) { KnownHostsRepository(parentLink).load() }; check(parentLink.delete())
        } finally { root.deleteRecursively() }
    }
    @Test fun readOnlyRecordsCannotBeRemovedThroughRepository() {
        known.writeText("@revoked ${line()}")
        val repo=KnownHostsRepository(directory); val snapshot=repo.load(); val bytes=known.readBytes()
        assertThrows(IllegalStateException::class.java) { repo.remove(snapshot,snapshot.records.single().id) }
        assertThrows(IllegalStateException::class.java) { repo.remove(snapshot,"not-a-record") }
        assertArrayEquals(bytes,known.readBytes())
    }
    @Test fun lateReadAfterLeavingCannotReplaceNewSnapshot() {
        val entered=CountDownLatch(1); val release=CountDownLatch(1); val ended=CountDownLatch(1); val reads=AtomicInteger()
        val old=KnownHostsSnapshot(line("old.example").toByteArray(),"old")
        val fresh=KnownHostsSnapshot(line("new.example").toByteArray(),"new")
        inject(object:KnownHostsRepository(directory) {
            override fun load():KnownHostsSnapshot {
                check(Looper.myLooper()!=Looper.getMainLooper())
                if(reads.incrementAndGet()==1) {
                    entered.countDown(); check(release.await(10,TimeUnit.SECONDS)); ended.countDown(); return old
                }
                return fresh
            }
        })
        try {
            main { hosts.knownHosts(); model.enter() }
            assertTrue(entered.await(5,TimeUnit.SECONDS))
            ui.onNodeWithText("返回").performClick()
            ui.onNodeWithText("主机验证记录").performScrollTo().performClick()
            await { !model.busy && model.snapshot?.revision=="new" }
            release.countDown(); assertTrue(ended.await(5,TimeUnit.SECONDS))
            SystemClock.sleep(250); ui.waitForIdle()
            assertEquals("new",main { model.snapshot?.revision }); assertNull(main { model.error })
        } finally { release.countDown() }
    }
    @Test fun committedConflictAndUnknownResultsRemainDistinctAndReload() {
        val results=listOf(KnownHostsUpdate(3,true,false,"verify fault"),KnownHostsUpdate(3,false,true,"conflict"),
            KnownHostsUpdate(3,false,false,"write fault"),null)
        val expected=listOf("记录修改已提交","记录已变化","本次移除未提交","移除结果无法确认")
        val calls=AtomicInteger()
        inject(object:KnownHostsRepository(directory) {
            override fun remove(snapshot:KnownHostsSnapshot,id:String):KnownHostsUpdate {
                check(Looper.myLooper()!=Looper.getMainLooper())
                val result=results[calls.getAndIncrement()]
                if(result==null) throw IllegalStateException("bridge failure")
                if(result.committed) known.writeText(line("committed.example"))
                return result
            }
        })
        known.writeText(line()); settings()
        for(index in results.indices) {
            open(); request(); confirm()
            ui.onNodeWithTag("known-hosts-notice").assertTextContains(expected[index],substring=true)
            assertEquals("committed.example",main { model.snapshot!!.records.single().record.hosts })
        }
        assertEquals(4,calls.get())
    }
    @Test fun confirmedWriteSurvivesLeavingAndReentryWithoutDuplicateSubmission() {
        val started=CountDownLatch(1); val release=CountDownLatch(1); val calls=AtomicInteger()
        inject(object:KnownHostsRepository(directory) {
            override fun remove(snapshot:KnownHostsSnapshot,id:String):KnownHostsUpdate {
                calls.incrementAndGet(); started.countDown(); check(release.await(10,TimeUnit.SECONDS))
                return super.remove(snapshot,id)
            }
        })
        known.writeText(line()); settings(); open(); request()
        try {
            ui.onNodeWithText("确认移除记录").performClick(); assertTrue(started.await(5,TimeUnit.SECONDS))
            main { model.remove() }; assertEquals(1,calls.get())
            ui.onNodeWithText("返回").performClick(); ui.onNodeWithText("返回").performClick()
            assertEquals(Page.SETTINGS,main { hosts.page })
            ui.onNodeWithText("主机验证记录").performScrollTo().performClick()
            assertTrue(main { model.writing }); ui.onNodeWithText("重新载入记录").assertIsNotEnabled()
            release.countDown(); await { !model.busy && model.snapshot!=null }
            assertEquals(1,calls.get()); assertEquals("",known.readText())
            ui.onNodeWithTag("known-hosts-notice").assertTextEquals("记录已移除")
            ui.onNodeWithTag("known-hosts-empty").assertExists()
        } finally { release.countDown() }
    }
    private fun find(view:View):TerminalView? {
        if(view is TerminalView) return view
        if(view is ViewGroup) for(i in 0 until view.childCount) find(view.getChildAt(i))?.let{return it}
        return null
    }
    private fun terminal()=checkNotNull(find(ui.activity.window.decorView))
    private fun pid(prefix:String):String?=terminal().currentFrame?.let { frame ->
        (0 until frame.rows).map(frame::line).firstOrNull { it.startsWith(prefix) && it.removePrefix(prefix).matches(Regex("[0-9]+")) }
    }
    @Test fun realTrustRemovalPromptsNewConnectionAndKeepsExistingRemotePid() {
        val vault=IdentityVault(File(ui.activity.filesDir,"identities"))
        val key=vault.import("known-hosts-product",fixture.getString("pem").toByteArray()); identity=key
        val alias="trust-product"
        config.writeText("Host $alias\n HostName 127.0.0.1\n Port ${fixture.getInt("port")}\n User ${fixture.getString("user")}\n")
        val savedConfig=config.readBytes()
        main { hosts.reload() }; await { !hosts.busy }
        ui.onNodeWithText(alias).performClick(); await { sessions.connecting?.auth?.phase==SSHAuthPhase.HOSTKEY }
        ui.onNodeWithText("信任并连接").performClick(); await { sessions.connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        ui.onNodeWithTag("ssh-key-${key.id}").performScrollTo().performClick()
        ui.onNodeWithText("认证并连接").performClick(); await { sessions.active?.state?.connected==true && sessions.connecting==null }
        await { find(ui.activity.window.decorView)?.currentFrame!=null }
        val connected=main { sessions.active!! }
        main { terminal().commitText("stty -echo; PS1=''; printf '\\033[2J\\033[HTRUST:%s\\r\\n' \"\u0024\u0024\"\r") }
        await("remote PID before removing trust") { pid("TRUST:")!=null }
        val before=main { pid("TRUST:")!! }
        ui.onNodeWithTag("terminal-back").performClick(); settings(); open()
        ui.onNodeWithTag("known-host-fingerprint").performScrollTo().assertTextEquals(fixture.getString("fingerprint"))
        request(); confirm(); assertEquals("",known.readText())
        assertArrayEquals(savedConfig,config.readBytes()); assertTrue(vault.list().any { it.id==key.id })
        assertSame(connected,main { sessions.active }); assertTrue(main { connected.state.connected })
        ui.onNodeWithText("返回").performClick(); ui.onNodeWithText("返回").performClick()
        main { sessions.selectNewRemote() }
        ui.onNodeWithText(alias).performClick(); await("new connection asks for host verification") { sessions.connecting?.auth?.phase==SSHAuthPhase.HOSTKEY }
        main { sessions.cancelConnection(sessions.connecting!!.id); sessions.show(connected.id) }
        await { find(ui.activity.window.decorView)?.currentFrame!=null }
        main { terminal().commitText("printf '\\033[2J\\033[HAFTER:%s\\r\\n' \"\u0024\u0024\"\r") }
        await("existing remote PID and IO retained") { pid("AFTER:")==before.replace("TRUST:","AFTER:") }
        assertEquals(1,main { sessions.sessions.size })
    }
}
