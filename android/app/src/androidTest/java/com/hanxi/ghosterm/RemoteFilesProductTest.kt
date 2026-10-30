package com.hanxi.ghosterm

import android.os.SystemClock
import android.graphics.Bitmap
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.data.IdentityVault
import com.hanxi.ghosterm.data.StoredIdentity
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import java.net.Socket
import org.json.JSONObject
import com.hanxi.ghosterm.domain.FileKind

/** Actual file page, current configuration, Keystore authentication and isolated real SFTP. */
@RunWith(AndroidJUnit4::class)
class RemoteFilesProductTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private val fixture by lazy { sshFixture() }
    private fun files()=ViewModelProvider(ui.activity)[FilesViewModel::class.java]
    private fun sessions()=ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
    private fun hosts()=ViewModelProvider(ui.activity)[HostsViewModel::class.java]
    private lateinit var config:File
    private lateinit var known:File
    private var configBefore:ByteArray?=null
    private var knownBefore:ByteArray?=null
    private lateinit var vault:IdentityVault
    private lateinit var key:StoredIdentity
    private val extraKeys=mutableListOf<StoredIdentity>()
    private val alias="files-${UUID.randomUUID().toString().take(8)}"
    @Before fun setup() {
        config=File(ui.activity.filesDir,"home/.ssh/config");known=File(ui.activity.filesDir,"home/.ssh/known_hosts")
        configBefore=config.takeIf { it.exists() }?.readBytes();knownBefore=known.takeIf { it.exists() }?.readBytes()
        known.delete();vault=IdentityVault(File(ui.activity.filesDir,"identities"))
        key=vault.import("Files ${UUID.randomUUID()}",fixture.getString("filePem").toByteArray())
        load()
    }
    private fun load(extra:String="",user:String=fixture.getString("user")) {
        config.parentFile!!.mkdirs()
        config.writeText("Host $alias\n HostName 127.0.0.1\n Port ${fixture.getInt("port")}\n${if(user.isEmpty()) "" else " User $user\n"} IdentityFile ghosterm-identity:${key.id}\n$extra")
        ui.runOnIdle { hosts().reload() }
        ui.waitUntil(10000) { !hosts().busy && hosts().snapshot?.text==config.readText() }
    }
    @After fun cleanup() {
        ui.runOnIdle {
            sessions().connecting?.let { sessions().cancelConnection(it.id) };files().disconnectRemote();files().leave()
            sessions().sessions.map { it.id }.forEach(sessions()::close)
        }
        val end=SystemClock.uptimeMillis()+10000
        while(owners()!=0 && SystemClock.uptimeMillis()<end) SystemClock.sleep(20)
        assertEquals("file page releases its owners",0,owners())
        if(::key.isInitialized) vault.remove(key)
        extraKeys.forEach(vault::remove)
        if(::config.isInitialized) { configBefore?.let(config::writeBytes) ?: config.delete();knownBefore?.let(known::writeBytes) ?: known.delete() }
    }
    private fun owners():Int {
        val type=Class.forName("com.hanxi.ghosterm.bridge.NativeFiles")
        return type.getDeclaredMethod("ownerCount").invoke(type.getField("INSTANCE").get(null)) as Int
    }
    private fun phase(phase:SSHAuthPhase) {
        try { ui.waitUntil(10000) { sessions().connecting?.auth?.phase==phase } }
        catch(failure:ComposeTimeoutException) { fail("file authentication did not reach $phase: ${sessions().connecting?.message}") }
    }
    private fun open() {
        ui.onNodeWithText("本机文件").performClick()
        ui.onNodeWithTag("file-location").performClick()
        ui.onNodeWithTag("file-connect:$alias").performScrollTo().performClick()
    }
    private fun login(trust:Boolean=true, expectedError:Boolean=false) {
        if(trust) { phase(SSHAuthPhase.HOSTKEY);ui.onNodeWithText("信任并连接").performClick() }
        phase(SSHAuthPhase.CREDENTIALS)
        ui.waitUntil(10000) { sessions().connecting?.identities?.any { it.id==key.id }==true }
        ui.onNodeWithTag("ssh-key-${key.id}").performScrollTo().performClick()
        ui.onNodeWithText("认证并连接").performClick()
        try { ui.waitUntil(10000) { sessions().connecting==null && files().location.endpoint.remoteAlias==alias && !files().loading } }
        catch(failure:ComposeTimeoutException) { fail("file page never ready: ${sessions().connecting?.message} / ${files().error}") }
        if(!expectedError) assertNull("real directory must succeed",files().error)
    }
    private fun capture(name:String) {
        val bitmap=ui.onRoot().captureToImage().asAndroidBitmap()
        File(ui.activity.cacheDir,"remote-files-$name.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG,100,it) }
    }
    private fun entry(name:String)=ui.onNodeWithTag("file-entry:$name")
    private fun control(op:String,user:String,stage:String="readdir",action:String="allow"):JSONObject {
        val peer=fixture.getJSONObject("channels")
        return Socket("127.0.0.1",peer.getInt("controlPort")).use { socket ->
            socket.soTimeout=5000
            socket.getOutputStream().write((JSONObject().put("token",peer.getString("token")).put("op",op)
                .put("user",user).put("stage",stage).put("action",action).toString()+"\n").toByteArray())
            JSONObject(socket.getInputStream().bufferedReader().readLine())
        }
    }
    private fun protocol(action:String):String {
        val user=control("new","",action=action).getString("user")
        val peer=fixture.getJSONObject("channels")
        extraKeys.add(key)
        key=vault.import("Protocol ${UUID.randomUUID()}",fixture.getString("pem").toByteArray())
        known.writeText("[127.0.0.1]:${peer.getInt("port")} ${peer.getString("public")}\n")
        config.writeText("Host $alias\n HostName 127.0.0.1\n Port ${peer.getInt("port")}\n User $user\n IdentityFile ghosterm-identity:${key.id}\n")
        ui.runOnIdle { hosts().reload() };ui.waitUntil { !hosts().busy && hosts().snapshot?.text==config.readText() }
        return user
    }
    @Test fun protocolReadFailureIsVisibleAndCannotPretendEmpty() {
        protocol("deny");open();login(false,expectedError=true)
        assertNotNull("directory failure must stay visible",files().error)
        ui.onNodeWithTag("file-list-error").assertIsDisplayed()
        ui.onNodeWithTag("file-list-empty").assertDoesNotExist()
        assertTrue(files().entries.isEmpty());ui.waitUntil { owners()==0 }
    }
    @Test fun cancellingBlockedDirectoryReleasesFileOwner() {
        val user=protocol("hold");open()
        phase(SSHAuthPhase.CREDENTIALS)
        ui.waitUntil { sessions().connecting?.identities?.any { it.id==key.id }==true }
        ui.onNodeWithTag("ssh-key-${key.id}").performScrollTo().performClick();ui.onNodeWithText("认证并连接").performClick()
        ui.waitUntil(10000) { sessions().connecting==null && files().loading && files().location.endpoint.remoteAlias==alias }
        ui.waitUntil(10000) { control("state",user).getJSONArray("events").toString().contains("readdir") }
        ui.onNodeWithText("返回").performClick()
        try { ui.waitUntil(3000) { owners()==0 } }
        catch(failure:ComposeTimeoutException) { fail("cancelled file page must release blocked owner") }
        assertFalse(files().visible);assertTrue(files().entries.isEmpty())
        control("release",user)
    }
    @Test fun remoteMetadataPreservesUnknownAndUnsignedSizes() {
        protocol("huge-size");open();login(false)
        assertEquals("/fixture",files().location.directoryId)
        val unknown=files().entries.single { it.name=="unknown" }
        assertNull("unknown size must stay unknown",unknown.size)
        assertEquals("unknown type must stay special",FileKind.OTHER,unknown.kind)
        assertEquals(0uL,files().entries.single { it.name=="zero" }.size)
        assertEquals(ULong.MAX_VALUE,files().entries.single { it.name=="huge" }.size)
        entry("unknown").performScrollTo();ui.onNodeWithText("特殊或未知类型").assertIsDisplayed()
        entry("huge").performScrollTo();ui.onNodeWithText("18446744073709551615 字节").assertIsDisplayed()
        entry("unknown").performScrollTo().performClick();ui.waitUntil { files().preview?.error!=null }
        ui.onNodeWithTag("file-preview").assertDoesNotExist()
    }
    @Test fun lateAuthenticationCannotReopenOrReplaceLeftPage() {
        open();phase(SSHAuthPhase.HOSTKEY);ui.onNodeWithText("信任并连接").performClick()
        phase(SSHAuthPhase.CREDENTIALS)
        ui.waitUntil { sessions().connecting?.identities?.any { it.id==key.id }==true }
        ui.runOnIdle { files().leave();files().enter() }
        ui.onNodeWithTag("ssh-key-${key.id}").performScrollTo().performClick();ui.onNodeWithText("认证并连接").performClick()
        try { ui.waitUntil(10000) { sessions().connecting==null && owners()==0 } }
        catch(failure:ComposeTimeoutException) { fail("late authenticated connection must be discarded and closed") }
        assertNull("late file handoff must not replace local endpoint",files().location.endpoint.remoteAlias)
        assertTrue(sessions().sessions.isEmpty())
    }
    @Test fun enteredUsernameIsKeptInImmutableFileIdentity() {
        load(user="");val bytes=config.readBytes();open()
        ui.onNodeWithTag("ssh-username").performTextInput(fixture.getString("user"))
        ui.onNodeWithText("继续连接").performClick();login()
        val identity=files().location.endpoint.remote!!
        assertEquals("file identity must use the entered username",fixture.getString("user"),identity.user)
        assertArrayEquals(bytes,config.readBytes())
        // Editing configuration does not rewrite an already connected endpoint's origin.
        load(user="another-user")
        assertEquals(identity,files().location.endpoint.remote)
        entry("文本.txt").performClick();ui.waitUntil { files().preview?.text!=null }
        ui.onNodeWithTag("file-preview").assertTextEquals("远端 UTF-8\nline two\n")
    }
    @Test fun realRemotePageAuthenticationRotationAndPreviewKeepTerminal() {
        ui.runOnIdle { sessions().newLocal() }
        ui.waitUntil(10000) { sessions().active?.state?.connected==true }
        val terminal=sessions().active!!
        ui.runOnIdle { terminal.surface.commit("stty -echo; printf '\\033[2J\\033[HFILE-PID:%s\\n' \"\u0024\u0024\"\r") }
        var pid=""
        ui.waitUntil(10000) {
            pid=terminal.connection.frame(-1)?.line(0) ?: ""
            pid.matches(Regex("FILE-PID:[0-9]+"))
        }
        ui.runOnIdle { sessions().home() }
        open();phase(SSHAuthPhase.HOSTKEY)
        ui.onNodeWithTag("ssh-fingerprint").assertTextEquals(fixture.getString("fingerprint"))
        val request=sessions().connecting!!.id
        ui.activityRule.scenario.recreate()
        assertEquals("file auth request survives rotation",request,sessions().connecting!!.id)
        login()
        val endpoint=files().location.endpoint.id
        val identity=files().location.endpoint.remote!!
        assertEquals(alias,identity.alias);assertEquals("127.0.0.1",identity.host)
        assertEquals(fixture.getString("user"),identity.user);assertEquals(fixture.getInt("port"),identity.port)
        assertEquals("server canonical home must be displayed",fixture.getString("remoteRoot"),files().location.directoryId)
        assertEquals(7,files().entries.size)
        assertEquals("directories first",FileKind.DIRECTORY,files().entries.first().kind)
        capture("browse")
        ui.onNodeWithTag("file-location").assertTextContains("远端",substring=true)
        ui.onNodeWithTag("file-breadcrumb").assertTextContains(fixture.getString("remoteRoot"))
        entry("文本.txt").performClick()
        ui.waitUntil(10000) { files().preview?.text!=null }
        ui.onNodeWithTag("file-preview").assertTextEquals("远端 UTF-8\nline two\n")
        ui.activityRule.scenario.recreate()
        assertEquals(endpoint,files().location.endpoint.id)
        ui.onNodeWithTag("file-preview").assertTextEquals("远端 UTF-8\nline two\n")
        ui.onNodeWithText("返回").performClick()
        entry("文本.txt").performTouchInput { longClick() }
        assertNull(files().preview);assertEquals(1,files().selected.size);capture("selected")
        ui.onNodeWithText("取消选择").performClick()
        entry("folder").performClick();ui.waitUntil { !files().loading }
        ui.onNodeWithTag("file-list-empty").assertIsDisplayed()
        ui.onNodeWithText("返回").performClick();ui.waitUntil { !files().loading }
        ui.onNodeWithText("返回").performClick()
        try { ui.waitUntil(3000) { !files().visible && owners()==0 } }
        catch(failure:ComposeTimeoutException) { fail("leaving file page must close idle owner") }
        assertSame(terminal,sessions().active)
        assertEquals("file login must not add a terminal",1,sessions().sessions.size)
        ui.runOnIdle { terminal.surface.commit("printf '\\033[2J\\033[HAFTER-PID:%s\\n' \"\u0024\u0024\"\r") }
        ui.waitUntil(10000) { terminal.connection.frame(-1)?.line(0)==pid.replace("FILE-PID:","AFTER-PID:") }
    }
    @Test fun remoteKindsAndLimitsNeverPublishMisleadingPreview() {
        open();login()
        for(name in listOf("link","binary","invalid","large")) {
            entry(name).performScrollTo().performClick()
            ui.waitUntil(10000) { files().preview?.error!=null }
            ui.onNodeWithTag("file-preview-error").assertIsDisplayed()
            ui.onNodeWithTag("file-preview").assertDoesNotExist()
            ui.onNodeWithText("返回").performClick()
        }
        entry("zero").performScrollTo().performClick()
        ui.waitUntil { files().preview?.text!=null };assertEquals("",files().preview!!.text)
        ui.onNodeWithText("返回").performClick()
        ui.onNodeWithText("更多").performClick();ui.onNodeWithText("断开文件连接").performClick()
        ui.waitUntil { owners()==0 && !files().loading }
        assertNull(files().location.endpoint.remoteAlias)
        assertTrue(sessions().sessions.isEmpty())
    }
    @Test fun configurationGuardAndRejectedTrustKeepLocalLocation() {
        load(" ProxyJump forbidden.example\n");open()
        ui.waitUntil { hosts().error!=null }
        ui.onNodeWithTag("file-connect-error").assertTextContains("不支持 SSH 配置",substring=true)
        assertNull(sessions().connecting);assertNull(files().location.endpoint.remoteAlias);assertEquals(0,owners())
        ui.runOnIdle { files().dismissLocations();files().leave() }
        load();open();phase(SSHAuthPhase.HOSTKEY)
        ui.onNodeWithText("拒绝").performClick()
        ui.waitUntil { sessions().connecting?.failed==true }
        ui.onNodeWithTag("ssh-failure").assertTextContains("拒绝",substring=true)
        assertFalse(known.exists());assertNull(files().location.endpoint.remoteAlias)
        assertTrue(sessions().sessions.isEmpty())
    }
    @Test fun leavingRequiresExplicitReconnectWithNewEndpoint() {
        open();login();val original=files().location.endpoint.id
        ui.onNodeWithText("返回").performClick();ui.waitUntil { owners()==0 && !files().visible }
        load(" ProxyJump forbidden.example\n")
        ui.onNodeWithText("本机文件").performClick();ui.waitUntil { files().error!=null }
        ui.onNodeWithTag("file-list-error").assertTextContains("重新连接",substring=true)
        ui.onNodeWithTag("file-list-empty").assertDoesNotExist()
        assertEquals(0,owners());assertEquals(original,files().location.endpoint.id)
        ui.onNodeWithText("重新连接").performClick();ui.waitUntil { hosts().error!=null }
        ui.onNodeWithTag("file-connect-error").assertTextContains("不支持 SSH 配置",substring=true)
        assertNull(sessions().connecting)
        load()
        ui.onNodeWithText("重新连接").performClick();login(false)
        assertNotEquals("reconnection must get a fresh endpoint identity",original,files().location.endpoint.id)
        entry("文本.txt").assertIsDisplayed()
    }
}
