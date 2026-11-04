package com.hanxi.ghosterm

import android.view.View
import android.view.ViewGroup
import com.hanxi.ghosterm.terminal.TerminalView
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import androidx.compose.runtime.snapshotFlow
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.SSHAuthPhase
import com.hanxi.ghosterm.data.IdentityVault
import com.hanxi.ghosterm.data.StoredIdentity
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

/** Real production Activity, real native owner, real remote shell and Android Keystore. */
@RunWith(AndroidJUnit4::class)
class SSHProductTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val fixture by lazy { sshFixture() }
    private fun sessions() = ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
    private fun hosts() = ViewModelProvider(ui.activity)[HostsViewModel::class.java]
    private lateinit var config: File
    private lateinit var known: File
    private var beforeConfig: ByteArray? = null
    private var beforeKnown: ByteArray? = null
    private lateinit var vault: IdentityVault
    private lateinit var identity: StoredIdentity
    private val alias = "fixture-${UUID.randomUUID().toString().take(8)}"
    @Before fun setup() {
        config = File(ui.activity.filesDir,"home/.ssh/config")
        known = File(ui.activity.filesDir,"home/.ssh/known_hosts")
        beforeConfig = config.takeIf { it.exists() }?.readBytes()
        beforeKnown = known.takeIf { it.exists() }?.readBytes()
        known.delete()
        vault = IdentityVault(File(ui.activity.filesDir,"identities"))
        identity = vault.import("Test ${UUID.randomUUID()}",fixture.getString("pem").toByteArray())
        load()
    }
    private fun load(user: String = fixture.getString("user"), extra: String = "") {
        config.parentFile!!.mkdirs()
        config.writeText("Host $alias\n HostName 127.0.0.1\n Port ${fixture.getInt("port")}\n" + if(user.isEmpty()) extra else " User $user\n$extra")
        ui.runOnIdle { hosts().reload() }
        ui.waitUntil(10000) { !hosts().busy && hosts().snapshot?.text == config.readText() }
    }
    @After fun cleanup() {
        ui.runOnIdle {
            sessions().connecting?.let { sessions().cancelConnection(it.id) }
            sessions().sessions.map { it.id }.forEach(sessions()::close)
        }
        if (::identity.isInitialized) vault.remove(identity)
        if (::config.isInitialized) { beforeConfig?.let(config::writeBytes) ?: config.delete(); beforeKnown?.let(known::writeBytes) ?: known.delete() }
    }
    private fun find(view: View): TerminalView? {
        if (view is TerminalView) return view
        if (view is ViewGroup) for (i in 0 until view.childCount) find(view.getChildAt(i))?.let { return it }
        return null
    }
    private fun terminal() = checkNotNull(find(ui.activity.window.decorView))
    private fun phase(value: SSHAuthPhase) {
        try { ui.waitUntil(15000) { sessions().connecting?.let { it.auth?.phase == value && (it.auth?.request ?: 0) > 0 &&
            (value != SSHAuthPhase.CREDENTIALS || !it.passiveProgress) } == true } }
        catch (failure: ComposeTimeoutException) { fail("SSH page did not reach $value: ${sessions().connecting?.message}") }
    }
    private fun open() { ui.onNodeWithText(alias).performClick(); phase(SSHAuthPhase.HOSTKEY) }
    private fun login(trust: Boolean = true): UUID {
        if (trust) ui.onNodeWithText("信任并连接").performClick()
        phase(SSHAuthPhase.CREDENTIALS)
        ui.onNodeWithTag("ssh-key-${identity.id}").performScrollTo().performClick()
        // Keep these owner/navigation scenarios on their deliberate manual-authentication path.
        ui.onNodeWithTag("ssh-remember-identity").performScrollTo().performClick()
        ui.onNodeWithTag("ssh-secret").assertDoesNotExist()
        ui.onNodeWithText("认证并连接").performClick()
        ui.waitUntil(15000) { sessions().visible && sessions().active?.state?.connected == true && sessions().connecting == null }
        ui.onNodeWithTag("terminal-surface").assertIsDisplayed()
        ui.waitUntil(10000) { find(ui.activity.window.decorView)?.currentFrame != null }
        return sessions().active!!.id
    }
    @Test fun stoppedSshRechecksConfigurationAndAuthenticatesANewOwnerWithoutRemovingHistory() {
        open(); val oldId = login()
        val old = sessions().active!!
        ui.runOnIdle { terminal().commitText("printf '\\r\\nSTOPPED_SSH_PROOF\\r\\n'; exit 7\r") }
        ui.waitUntil(15_000) { old.state.terminal }
        val stoppedState = old.state
        assertTrue(stoppedState.exitCode == null || stoppedState.exitCode == 7)
        ui.onNodeWithTag("terminal-recover").assertTextEquals("重新连接")
        load(extra = " ProxyCommand false\n")
        ui.onNodeWithTag("terminal-recover").performClick()
        ui.waitUntil(10_000) { !hosts().busy && hosts().error != null }
        ui.onNodeWithText(hosts().error!!).assertIsDisplayed()
        ui.runOnIdle { assertNull(sessions().connecting); assertSame(old, sessions().active) }
        load()
        ui.onNodeWithTag("terminal-recover").performClick()
        phase(SSHAuthPhase.CREDENTIALS)
        ui.runOnIdle { sessions().cancelConnection(sessions().connecting!!.id) }
        ui.onNodeWithTag("terminal-stopped").assertIsDisplayed()
        ui.runOnIdle { assertSame(old, sessions().active); assertEquals(1, sessions().sessions.size) }
        ui.onNodeWithTag("terminal-recover").performClick()
        val newId = login(trust = false)
        assertNotEquals(oldId, newId)
        ui.runOnIdle {
            assertEquals(2, sessions().sessions.size)
            assertTrue(sessions().sessions.any { it === old })
            assertNotSame(old.connection, sessions().active!!.connection)
            assertEquals(stoppedState, old.state)
            assertTrue(old.connection.frame(-1)!!.let { f -> (0 until f.rows).any { f.line(it).contains("STOPPED_SSH_PROOF") } })
        }
    }

    @Test fun deletingAHostKeepsItsEstablishedConnectionIdentityAndTrustRecord() {
        open(); val id = login()
        val session = sessions().active!!
        val trust = known.readBytes()
        val identityIds = vault.list().map { it.id }
        ui.terminalBack()
        ui.onNodeWithTag("host-menu:" + alias).performClick()
        ui.onNodeWithTag("host-delete:" + alias).performScrollTo().performClick()
        ui.onNodeWithTag("host-delete-confirm").performClick()
        ui.waitUntil(10_000) { !hosts().busy && hosts().snapshot?.hosts?.none { it.alias == alias } == true }
        assertArrayEquals(trust, known.readBytes())
        assertEquals(identityIds, vault.list().map { it.id })
        ui.runOnIdle {
            assertTrue(session.connection.state().connected)
            assertEquals(id, sessions().active!!.id)
            sessions().show(id)
            session.connection.commit("printf '\\r\\nHOST_DELETION_CONNECTION_PROOF\\r\\n'\r")
        }
        ui.waitUntil(10_000) { find(ui.activity.window.decorView)?.currentFrame?.let { f -> (0 until f.rows).any { f.line(it).contains("HOST_DELETION_CONNECTION_PROOF") } } == true }
    }

    @Test fun homeTrustKeyAuthenticationAndRotationKeepOneRemoteShell() {
        open()
        ui.onNodeWithTag("ssh-fingerprint").assertTextEquals(fixture.getString("fingerprint"))
        val attempt = sessions().connecting!!.id; val request = sessions().connecting!!.auth!!.request
        ui.activityRule.scenario.recreate()
        ui.runOnIdle { assertEquals(attempt,sessions().connecting!!.id); assertEquals(request,sessions().connecting!!.auth!!.request) }
        val id = login()
        assertTrue(known.readText().contains("[127.0.0.1]:${fixture.getInt("port")}"))
        ui.runOnIdle { terminal().commitText("stty -echo; PS1=''; printf '\\033[2J\\033[HPRODUCT:%s\\r\\n' \"\u0024\u0024\"\r") }
        var pid = ""
        try { ui.waitUntil(10000) {
            val frame = terminal().currentFrame
            pid = frame?.line(0) ?: ""
            pid.startsWith("PRODUCT:") && pid.removePrefix("PRODUCT:").all { it.isDigit() } && pid.length > 8
        } } catch (failure: ComposeTimeoutException) {
            val frame = terminal().currentFrame
            fail("remote PID not visible: " + frame?.let { (0 until it.rows).joinToString(" / ",transform = it::line) })
        }
        val owner = sessions()
        ui.activityRule.scenario.recreate()
        ui.waitUntil(10000) { find(ui.activity.window.decorView)?.currentFrame != null }
        ui.runOnIdle {
            assertSame(owner,sessions()); assertEquals(id,sessions().active!!.id)
            terminal().commitText("printf '\\033[2J\\033[HAFTER:%s\\r\\n' \"\u0024\u0024\"\r")
        }
        try { ui.waitUntil(10000) {
            terminal().currentFrame?.let { frame -> (0 until frame.rows).any { frame.line(it) == pid.replace("PRODUCT:","AFTER:") } } == true
        } } catch (failure: ComposeTimeoutException) {
            val frame = terminal().currentFrame
            fail("remote PID after recreation, expected $pid: " + frame?.let { (0 until it.rows).joinToString(" / ",transform = it::line) })
        }
        ui.terminalBack()
        ui.onNodeWithText(alias).performClick()
        ui.runOnIdle { assertEquals(id,sessions().active!!.id); assertEquals(1,sessions().sessions.size) }
    }
    @Test fun refusalNeverRequestsCredentialsOrWritesTrust() {
        open(); ui.onNodeWithText("拒绝").performClick()
        try { ui.waitUntil(10000) { sessions().connecting?.failed == true } }
        catch (failure: ComposeTimeoutException) { fail("拒绝后没有进入失败状态") }
        ui.onNodeWithTag("ssh-failure").assertTextContains("拒绝", substring = true)
        ui.onNodeWithText("认证并连接").assertDoesNotExist()
        assertFalse(known.exists()); assertTrue(sessions().sessions.isEmpty())
        ui.onNodeWithText("关闭").performClick()
        ui.onNodeWithText(alias).assertIsDisplayed()
    }
    @Test fun missingUsernameAndCancelledTrustLeaveConfigurationUnchanged() {
        load(user = "")
        val bytes = config.readBytes()
        ui.onNodeWithText(alias).performClick()
        ui.onNodeWithTag("ssh-username").performTextInput(fixture.getString("user"))
        ui.onNodeWithText("继续连接").performClick()
        phase(SSHAuthPhase.HOSTKEY)
        ui.onNodeWithText("取消连接").performClick()
        ui.waitUntil(10000) { sessions().connecting == null }
        assertArrayEquals(bytes,config.readBytes()); assertFalse(known.exists()); assertTrue(sessions().sessions.isEmpty())
    }
    @Test fun explicitNewConnectionAndHostChoiceDoNotReplaceLocalOrRemoteOwners() {
        open(); val first = login()
        ui.openTerminalSessions()
        ui.onNodeWithText("新建 SSH 会话").performClick()
        ui.onNodeWithText(alias).performClick()
        val second = login(trust = false)
        assertNotEquals(first,second); assertEquals(2,sessions().sessions.size)
        ui.terminalBack()
        ui.onNodeWithText(alias).performClick()
        ui.onNodeWithText("选择 $alias 的会话").assertIsDisplayed()
        ui.onNodeWithTag("ssh-session-$first").performClick()
        ui.runOnIdle { assertEquals(first,sessions().active!!.id); assertEquals(2,sessions().sessions.size) }
        ui.terminalBack()
        ui.onNodeWithText("本地终端").performClick()
        ui.waitUntil(10000) { sessions().active?.remoteAlias == null && sessions().active?.state?.connected == true }
        assertEquals(3,sessions().sessions.size)
        assertTrue(sessions().sessions.filter { it.remoteAlias != null }.all { it.state.connected })
    }
    @Test fun unsupportedConfigNeverStartsConnectionOrRequestsCredentials() {
        load(extra = " ProxyJump unsupported\n")
        ui.onNodeWithText(alias).performClick()
        ui.waitUntil(10000) { !hosts().busy }
        assertTrue(hosts().error!!.contains("proxyjump"))
        assertNull(sessions().connecting); assertTrue(sessions().sessions.isEmpty()); assertFalse(known.exists())
        ui.onNodeWithText("认证并连接").assertDoesNotExist()
    }
    @Test fun configuredKeyKeepsCredentialStagesInlineAndCreatesOneShell() {
        load(extra = " IdentityFile ${com.hanxi.ghosterm.data.IdentityReferences.encode(identity.id)}\n")
        val credentialStages = mutableListOf<Pair<Boolean, String>>()
        lateinit var observer: Job
        ui.runOnIdle {
            observer = ui.activity.lifecycleScope.launch(start = CoroutineStart.UNDISPATCHED) {
                snapshotFlow { sessions().connecting?.let {
                    it.auth?.phase to (it.passiveProgress to "request=${it.auth?.request} auto=${it.automaticAuthentication} busy=${it.credentialBusy} error=${it.credentialError != null}")
                } }.collect { state -> if (state?.first == SSHAuthPhase.CREDENTIALS) credentialStages.add(state.second) }
            }
        }
        try {
            open()
            ui.onAllNodes(isDialog()).assertCountEquals(1)
            ui.onNodeWithTag("ssh-connection-progress").assertDoesNotExist()
            ui.onNodeWithText("信任并连接").performClick()
            ui.waitUntil(15000) { sessions().connecting == null && sessions().active?.state?.connected == true }
            ui.onNodeWithTag("terminal-surface").assertIsDisplayed()
            ui.onAllNodes(isDialog()).assertCountEquals(0)
            ui.runOnIdle {
                assertTrue("The real credential stage must be observed", credentialStages.isNotEmpty())
                assertTrue("Configured key attempts must not expose a manual credential form: $credentialStages", credentialStages.all { it.first })
                assertEquals(1, sessions().sessions.size)
            }
        } finally { observer.cancel() }
    }
}
