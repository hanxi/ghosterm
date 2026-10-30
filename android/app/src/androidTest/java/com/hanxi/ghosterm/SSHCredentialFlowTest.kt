package com.hanxi.ghosterm

import android.os.SystemClock
import android.view.View
import android.view.ViewGroup
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.SSHAuthPhase
import com.hanxi.ghosterm.data.IdentityVault
import com.hanxi.ghosterm.data.StoredIdentity
import com.hanxi.ghosterm.terminal.TerminalView
import org.json.JSONObject
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Real OpenSSH servers: host public-key fixture plus disposable password accounts in Docker. */
@RunWith(AndroidJUnit4::class)
class SSHCredentialFlowTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val fixture by lazy { sshFixture() }
    private lateinit var model: SessionsViewModel
    private lateinit var hosts: HostsViewModel
    private lateinit var vault: IdentityVault
    private lateinit var identity: StoredIdentity
    private lateinit var config: File
    private lateinit var known: File
    private var beforeConfig: ByteArray? = null
    private var beforeKnown: ByteArray? = null
    private val alias = "credentials-${UUID.randomUUID().toString().take(8)}"
    private fun <T> onMain(block: () -> T): T {
        var result: T? = null
        instrumentation.runOnMainSync { result=block() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun await(predicate: () -> Boolean) {
        val deadline=SystemClock.elapsedRealtime()+15_000
        while(SystemClock.elapsedRealtime()<deadline) {
            if(onMain(predicate)) return
            SystemClock.sleep(25)
        }
        fail("Credential flow did not reach expected state")
    }
    @Before fun setup() {
        onMain { model=ViewModelProvider(ui.activity)[SessionsViewModel::class.java]; hosts=ViewModelProvider(ui.activity)[HostsViewModel::class.java] }
        config=File(ui.activity.filesDir,"home/.ssh/config"); known=File(ui.activity.filesDir,"home/.ssh/known_hosts")
        beforeConfig=config.takeIf(File::exists)?.readBytes(); beforeKnown=known.takeIf(File::exists)?.readBytes()
        known.delete()
        vault=IdentityVault(File(ui.activity.filesDir,"identities"))
        identity=vault.import("Encrypted test key",fixture.getString("encryptedPem").toByteArray())
    }
    @After fun cleanup() {
        if(::model.isInitialized) onMain { model.connecting?.let { model.cancelConnection(it.id) }; model.sessions.map { it.id }.forEach(model::close) }
        if(::identity.isInitialized) vault.remove(identity)
        if(::config.isInitialized) { beforeConfig?.let(config::writeBytes) ?: config.delete(); beforeKnown?.let(known::writeBytes) ?: known.delete() }
    }
    private fun open(server: JSONObject = fixture) {
        config.parentFile!!.mkdirs()
        config.writeText("Host $alias\n HostName 127.0.0.1\n Port ${server.getInt("port")}\n User ${server.getString("user")}\n")
        onMain { hosts.reload() }
        await { !hosts.busy && hosts.snapshot?.text==config.readText() }
        ui.onNodeWithText(alias).performClick()
        await { model.connecting?.auth?.phase==SSHAuthPhase.HOSTKEY || model.connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        if(onMain { model.connecting?.auth?.phase==SSHAuthPhase.HOSTKEY }) {
            ui.onNodeWithTag("ssh-fingerprint").assertTextEquals(server.getString("fingerprint"))
            ui.onNodeWithText("信任并连接").performClick()
        }
        await { model.connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS && model.connecting!!.auth!!.request>0 }
    }
    private fun chooseKey() { ui.onNodeWithTag("ssh-key-${identity.id}").performScrollTo().performClick() }
    private fun typeSecret(secret: String) { ui.onNodeWithTag("ssh-secret").performScrollTo().performTextInput(secret) }
    private fun assertSecretEmpty() {
        // assertTextEquals would print the actual credential on a regression.
        val value=ui.onNodeWithTag("ssh-secret").fetchSemanticsNode().config[SemanticsProperties.EditableText].text
        assertTrue("A credential survived a request, mode or Activity boundary",value.isEmpty())
    }
    private fun retryAfterWrongSecret() {
        val previous=onMain { model.connecting!!.auth!!.request }
        typeSecret("deliberately-wrong")
        ui.onNodeWithText("认证并连接").performClick()
        await { model.connecting?.let { it.failed || (it.auth?.phase==SSHAuthPhase.CREDENTIALS && it.auth!!.request>previous && it.auth!!.error==4) }==true }
        assertFalse("Credential rejection must allow a fresh request",onMain { model.connecting!!.failed })
        ui.onNodeWithText("认证失败，请重新提供凭据").assertIsDisplayed()
        assertTrue(onMain { model.sessions.isEmpty() })
    }
    private fun find(view: View): TerminalView? {
        if(view is TerminalView) return view
        if(view is ViewGroup) for(i in 0 until view.childCount) find(view.getChildAt(i))?.let { return it }
        return null
    }
    private fun connected() {
        ui.onNodeWithText("认证并连接").performClick()
        await { model.connecting==null && model.active?.state?.connected==true }
        ui.onNodeWithTag("terminal-surface").assertIsDisplayed()
        await { find(ui.activity.window.decorView)?.currentFrame!=null }
        onMain { find(ui.activity.window.decorView)!!.commitText("stty -echo; PS1=''; printf '\\033[2J\\033[HCREDENTIAL:%s\\r\\n' \"\u0024\u0024\"\r") }
        await { find(ui.activity.window.decorView)?.currentFrame?.let { frame ->
            (0 until frame.rows).any { Regex("CREDENTIAL:[0-9]+").matches(frame.line(it)) }
        }==true }
        val secrets=listOf(fixture.getString("passphrase"),fixture.getJSONObject("password").getString("password"),fixture.getJSONObject("mixed").getString("password"))
        for(file in listOf(config,known,File(ui.activity.filesDir,"identities/${identity.id}.json"))) {
            val text=file.readText()
            assertTrue("An authentication secret was persisted",secrets.none(text::contains))
        }
    }
    @Test fun encryptedPrivateKeyRetriesWithFreshRequestAndCorrectPassphrase() {
        assertTrue("Encrypted fixture must request a passphrase",identity.needsPassphrase)
        open(); chooseKey(); retryAfterWrongSecret()
        chooseKey(); assertSecretEmpty()
        typeSecret(fixture.getString("passphrase")); connected()
    }
    @Test fun passwordOnlyRetriesAndActivityRecreationClearsTypedPassword() {
        val server=fixture.getJSONObject("password")
        open(server)
        assertEquals(2,onMain { model.connecting!!.auth!!.methods })
        ui.onNodeWithText("使用密钥").assertDoesNotExist()
        retryAfterWrongSecret(); assertSecretEmpty()
        typeSecret(server.getString("password"))
        val attempt=onMain { model.connecting!!.id }; val request=onMain { model.connecting!!.auth!!.request }
        ui.activityRule.scenario.recreate()
        onMain { assertSame(model,ViewModelProvider(ui.activity)[SessionsViewModel::class.java]); assertEquals(attempt,model.connecting!!.id); assertEquals(request,model.connecting!!.auth!!.request) }
        assertSecretEmpty(); typeSecret(server.getString("password")); connected()
    }
    @Test fun switchingKeyAndPasswordModesNeverCarriesTheOtherSecret() {
        val server=fixture.getJSONObject("mixed")
        open(server)
        assertEquals(3,onMain { model.connecting!!.auth!!.methods })
        chooseKey(); typeSecret(fixture.getString("passphrase"))
        ui.onNodeWithText("使用密码").performClick(); assertSecretEmpty()
        typeSecret(server.getString("password"))
        ui.onNodeWithText("使用密钥").performClick(); assertSecretEmpty()
        ui.onNodeWithText("使用密码").performClick(); assertSecretEmpty()
        typeSecret(server.getString("password")); connected()
    }
    @Test fun cancelledPasswordRequestErasesLateMaterialAndNewPromptStartsEmpty() {
        val server=fixture.getJSONObject("password")
        open(server)
        val id=onMain { model.connecting!!.id }; val request=onMain { model.connecting!!.auth!! }
        typeSecret(server.getString("password")); ui.onNodeWithText("取消连接").performClick()
        await { model.connecting==null }
        open(server); assertSecretEmpty()
        val late=server.getString("password").toByteArray()
        onMain { model.password(id,request,late) }
        assertTrue("Cancelled request retained credential bytes",late.all { it==0.toByte() })
        assertNotEquals(id,onMain { model.connecting!!.id })
        typeSecret(server.getString("password")); connected()
    }
}
