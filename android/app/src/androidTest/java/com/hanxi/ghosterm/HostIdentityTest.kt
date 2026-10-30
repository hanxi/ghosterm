package com.hanxi.ghosterm

import android.view.View
import android.view.ViewGroup
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ConfigBridge
import com.hanxi.ghosterm.bridge.SSHAuthPhase
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import com.hanxi.ghosterm.terminal.TerminalView
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.nio.file.Path
import java.security.KeyStore
import java.util.UUID

/** Configuration bytes, production controls, Keystore and an actual remote shell. */
@RunWith(AndroidJUnit4::class)
class HostIdentityTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val fixture by lazy { sshFixture() }
    private lateinit var hosts: HostsViewModel
    private lateinit var sessions: SessionsViewModel
    private lateinit var identities: IdentitiesViewModel
    private lateinit var vault: IdentityVault
    private lateinit var key: StoredIdentity
    private lateinit var other: StoredIdentity
    private lateinit var config: File
    private lateinit var known: File
    private lateinit var directory: File
    private lateinit var scratch: File
    private var oldIds = emptySet<String>()
    private var oldConfig: ByteArray? = null
    private var oldKnown: ByteArray? = null
    private val alias = "linked-${UUID.randomUUID().toString().take(8)}"
    private fun <T> main(action: () -> T): T {
        var value: T? = null
        instrumentation.runOnMainSync { value = action() }
        @Suppress("UNCHECKED_CAST") return value as T
    }
    private fun await(label: String, condition: () -> Boolean) {
        try { ui.waitUntil(15000) { main(condition) } }
        catch (_: ComposeTimeoutException) {
            fail(label + main { "; hostError=${hosts.error}; identityError=${identities.error}; connection=${sessions.connecting?.message}" })
        }
    }
    @Before fun setup() {
        main {
            hosts = ViewModelProvider(ui.activity)[HostsViewModel::class.java]
            sessions = ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
            identities = ViewModelProvider(ui.activity)[IdentitiesViewModel::class.java]
        }
        await("initial config") { !hosts.busy }
        directory = File(ui.activity.filesDir, "identities")
        oldIds = directory.listFiles().orEmpty().map { it.nameWithoutExtension }.toSet()
        vault = IdentityVault(directory)
        // Wrong key first: configuration must determine the offered/default key, not inventory order.
        other = vault.import("a-unrelated", fixture.getString("wrongPem").toByteArray())
        key = vault.import("z-linked", fixture.getString("pem").toByteArray())
        config = File(ui.activity.filesDir, "home/.ssh/config")
        known = File(ui.activity.filesDir, "home/.ssh/known_hosts")
        oldConfig = config.takeIf { it.exists() }?.readBytes(); oldKnown = known.takeIf { it.exists() }?.readBytes()
        known.delete()
        scratch = File(ui.activity.cacheDir, "host-identity-${UUID.randomUUID()}").apply { mkdirs() }
        load("")
    }
    private fun load(extra: String) {
        config.parentFile!!.mkdirs()
        config.writeText("Host $alias\n HostName 127.0.0.1\n User ${fixture.getString("user")}\n Port ${fixture.getInt("port")}\n$extra")
        main { hosts.reload() }
        await("configuration loaded") { !hosts.busy && hosts.snapshot?.text == config.readText() }
    }
    @After fun cleanup() {
        if (::sessions.isInitialized) main {
            identities.leave()
            sessions.connecting?.let { sessions.cancelConnection(it.id) }
            sessions.sessions.map { it.id }.forEach(sessions::close)
        }
        if (::config.isInitialized) { oldConfig?.let(config::writeBytes) ?: config.delete(); oldKnown?.let(known::writeBytes) ?: known.delete() }
        if (::directory.isInitialized) {
            val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
            directory.listFiles().orEmpty().filter { it.nameWithoutExtension !in oldIds }.forEach {
                store.deleteEntry("ghosterm.ssh.identity.${it.nameWithoutExtension}"); check(it.delete())
            }
        }
        if (::scratch.isInitialized) scratch.deleteRecursively()
    }
    private fun repository(text: String): ConfigRepository {
        File(scratch, "config").writeText(text)
        return ConfigRepository(scratch)
    }
    private fun editPage() {
        ui.onNodeWithText("编辑").performClick()
        await("identity editor ready") { hosts.page == Page.EDIT_HOST && !hosts.identityLoading }
        assertNull(main { hosts.identityError })
    }
    private fun settingsDetail() {
        ui.onNodeWithText("设置").performClick()
        ui.onNodeWithText("SSH 身份").performScrollTo().performClick()
        await("identity inventory") { !identities.busy }
        ui.onNodeWithTag("identity-row-${key.id}").performScrollTo().performClick()
        await("identity references") { !identities.busy }
    }
    @Test fun orderedIdentityEditsKeepInheritedIncludeAndOtherBytes() {
        val included = File(scratch,"included").apply { writeText("IdentityFile \"/keys/继承 #1\"\n") }
        val raw = "# header\r\nHost demo sibling\r\n Include ${included.path}\r\n IdentityFile first\r\n # untouched\r\n IdentityFile second\r\nHost *\r\n IdentityFile wildcard\r\nHost unrelated\r\n IdentityFile excluded\r\n"
        val repo = repository(raw); val base = repo.load()
        val values = listOf("/keys/中文 🔑 #1", "/keys/quote\"slash\\end", IdentityReferences.encode(key.id))
        val edited = try {
            repo.saveHost(base, HostEdit.from(base.hosts.first { it.alias=="demo" }).copy(identityFiles = values)).snapshot
        } catch (failure: Exception) { throw AssertionError("Valid quoted identity edit must succeed",failure) }
        val entries = ConfigBridge.identities(edited.bytes,"demo")
        assertEquals(values,entries.filter { it.explicitValue }.map { it.value })
        assertEquals(listOf("/keys/继承 #1","wildcard"),entries.filter { !it.explicitValue }.map { it.value })
        assertTrue(edited.text.contains(" # untouched\r\n")); assertTrue(edited.text.endsWith("Host unrelated\r\n IdentityFile excluded\r\n"))
        assertEquals(entries.map { it.value },ConfigBridge.identities(edited.bytes,"sibling").map { it.value })
        assertEquals(listOf("demo","sibling"),IdentityReferences.usages(edited,key.id).map { it.alias })
        assertArrayEquals(edited.bytes, repo.saveHost(edited, HostEdit.from(edited.hosts.first { it.alias=="demo" })).snapshot.bytes)
        val cleared = repo.saveHost(edited, HostEdit.from(edited.hosts.first { it.alias=="demo" }).copy(identityFiles=emptyList())).snapshot
        assertEquals(listOf("/keys/继承 #1","wildcard"),ConfigBridge.identities(cleared.bytes,"demo").map { it.value })
        assertEquals("IdentityFile \"/keys/继承 #1\"\n",included.readText())
    }
    @Test fun newHostRenameAndFinalLineUseOneConfigurationTransaction() {
        val repo = repository("# keep\n")
        val created = repo.saveHost(repo.load(),HostEdit(null,"before","127.0.0.1","user","22",identityFiles=listOf(IdentityReferences.encode(key.id)))).snapshot
        val renamed = repo.saveHost(created,HostEdit.from(created.hosts.single()).copy(alias="after",identityFiles=listOf("second"))).snapshot
        assertEquals(listOf("second"),ConfigBridge.identities(renamed.bytes,"after").map { it.value })
        assertTrue(renamed.text.startsWith("# keep\n")); assertFalse(renamed.text.contains("Host before"))
        File(scratch,"config").writeText("Host tail\n IdentityFile old")
        val tail = repo.load()
        val saved = repo.saveHost(tail,HostEdit.from(tail.hosts.single()).copy(identityFiles=listOf("new"))).snapshot
        assertEquals("new",ConfigBridge.identities(saved.bytes,"tail").single().value)
    }
    @Test fun identityEditRejectsCorruptEditorBackupFailureAndReadOnlyOwnership() {
        val original = "Host demo\n IdentityFile own\nHost *\n IdentityFile inherited\n"
        val repo = repository(original); val base = repo.load()
        val edit = HostEdit.from(base.hosts.single()).copy(identityFiles=listOf(IdentityReferences.encode(key.id)))
        val corrupt = ConfigRepository(scratch,editIdentityConfig = { bytes,name,values ->
            ConfigBridge.editIdentities(bytes,name,values).toString(Charsets.UTF_8).replace("IdentityFile inherited","IdentityFile damaged").toByteArray()
        })
        assertThrows(ConfigAccess::class.java) { corrupt.saveHost(base,edit) }
        val omitted = ConfigRepository(scratch,editIdentityConfig = { bytes,_,_ -> bytes })
        assertThrows(ConfigAccess::class.java) { omitted.saveHost(base,edit) }
        val failing = ConfigRepository(scratch,ops=object: ConfigFileOps() {
            override fun writeExclusive(path: Path,bytes: ByteArray) { throw java.io.IOException("fixture backup refused") }
        })
        assertThrows(ConfigAccess::class.java) { failing.saveHost(base,edit) }
        assertEquals(original,File(scratch,"config").readText())
        File(scratch,"config").writeText("Host demo\n IdentityFile first\nHost demo\n IdentityFile second\n")
        val duplicate = repo.load()
        assertThrows(ConfigAccess::class.java) { repo.saveHost(duplicate,HostEdit.from(duplicate.hosts.single()).copy(identityFiles=emptyList())) }
        val child=File(scratch,"child").apply { writeText("Host child\n IdentityFile kept\n") }
        File(scratch,"config").writeText("Include ${child.path}\n")
        val fromInclude=repo.load()
        assertThrows(ConfigAccess::class.java) { repo.saveHost(fromInclude,HostEdit.from(fromInclude.hosts.single()).copy(identityFiles=emptyList())) }
        assertEquals("Host child\n IdentityFile kept\n",child.readText())
    }
    @Test fun referenceCodecAndCandidatesNeverSubstituteUnrelatedKeys() {
        val reference = IdentityReferences.encode(key.id)
        assertEquals(key.id,IdentityReferences.decode(reference))
        assertNull(IdentityReferences.decode("$reference-extra"))
        assertNull(IdentityReferences.decode("ghosterm-identity:1-1-1-1-1"))
        val missing = IdentityReferences.encode(UUID.randomUUID().toString())
        val result=IdentityReferences.candidates(listOf(reference,reference,missing,"ghosterm-identity:invalid","/ordinary/key","none"),listOf(other,key))
        assertEquals(listOf(key),result.identities)
        assertEquals(4,result.issues.size)
        assertTrue(IdentityReferences.candidates(listOf(missing),listOf(other)).identities.isEmpty())
    }
    @Test fun identitySourceFailureKeepsScalarDraftAndNeverShowsEmptySuccess() {
        load(" IdentityFile one two\n")
        ui.onNodeWithText("编辑").performClick()
        await("source query failed") { !hosts.identityLoading }
        ui.onNodeWithTag("host-identity-error").performScrollTo().assertExists()
        ui.onNodeWithTag("host-add-identity-${key.id}").assertDoesNotExist()
        ui.onNodeWithText("别名").performScrollTo().performTextReplacement("$alias-edited")
        ui.onNodeWithText("重新读取身份").performScrollTo().performClick()
        await("source retry failed") { !hosts.identityLoading }
        assertEquals("$alias-edited",main { hosts.draft!!.edit.alias })
        ui.onNodeWithTag("host-identity-error").assertExists()
        assertTrue(config.readText().contains("IdentityFile one two"))
    }
    @Test fun editorDraftSurvivesRotationAndRestoreInheritancePreservesSource() {
        load(" IdentityFile own\nHost *\n IdentityFile ${IdentityReferences.encode(other.id)}\n")
        editPage()
        ui.onNodeWithTag("host-add-identity-${key.id}").performScrollTo().performClick()
        ui.activityRule.scenario.recreate()
        assertTrue(main { hosts.draft!!.dirty })
        ui.onNodeWithText("返回").performClick(); ui.onNodeWithText("继续编辑").performClick()
        ui.onNodeWithText("保存").performClick()
        await("identity-only draft saved") { !hosts.busy && hosts.page==Page.HOSTS }
        assertEquals(listOf("own",IdentityReferences.encode(key.id)),ConfigBridge.identities(config.readBytes(),alias).filter { it.explicitValue }.map { it.value })
        editPage(); ui.onNodeWithText("身份恢复继承").performScrollTo().performClick(); ui.onNodeWithText("保存").performClick()
        await("inheritance restored") { !hosts.busy && hosts.page==Page.HOSTS }
        assertEquals(listOf(IdentityReferences.encode(other.id)),ConfigBridge.identities(config.readBytes(),alias).map { it.value })
    }
    @Test fun referenceDetailsDistinguishConfigFailureFromZeroAndRefreshAfterRename() {
        load(" IdentityFile ${IdentityReferences.encode(key.id)}\n")
        settingsDetail()
        ui.onNodeWithTag("identity-usage-$alias").performScrollTo().assertExists()
        val renamed = "$alias-new"
        config.writeBytes(ConfigBridge.rename(config.readBytes(),alias,renamed))
        main { identities.open(identities.selected!!) }
        await("reference refresh") { !identities.busy }
        assertEquals(listOf(renamed),main { identities.usages.map { it.alias } })
        config.writeText("Host bad\n IdentityFile one two\n")
        main { identities.open(identities.selected!!) }
        await("reference read failed") { !identities.busy }
        ui.onNodeWithTag("identity-usage-error").performScrollTo().assertExists()
        ui.onNodeWithTag("identity-no-usages").assertDoesNotExist()
        assertNotNull(main { identities.info?.fingerprint })
        config.writeText("Host unreferenced\n")
        ui.onNodeWithText("重新读取引用").performScrollTo().performClick()
        await("reference recovery") { !identities.busy }
        ui.onNodeWithTag("identity-no-usages").performScrollTo().assertExists()
    }
    @Test fun malformedInventoryIdShowsAnErrorInsteadOfCrashingReferenceDetails() {
        File(directory,"not-a-uuid.json").writeText("damaged fixture")
        ui.onNodeWithText("设置").performClick()
        ui.onNodeWithText("SSH 身份").performScrollTo().performClick()
        await("damaged inventory") { !identities.busy }
        ui.onNodeWithTag("identity-row-not-a-uuid").performScrollTo().performClick()
        await("damaged reference details") { !identities.busy }
        ui.onNodeWithText("此条目的身份编号无效，不能用于主机关联。").performScrollTo().assertExists()
        ui.onNodeWithTag("identity-usage-error").performScrollTo().assertExists()
        ui.onNodeWithTag("identity-no-usages").assertDoesNotExist()
    }
    private fun phase(phase: SSHAuthPhase) = await("SSH phase $phase") { sessions.connecting?.auth?.let { it.phase==phase && it.request>0 } == true }
    @Test fun missingConfiguredIdentityRequiresExplicitOneTimeSelection() {
        load(" IdentityFile ${IdentityReferences.encode(UUID.randomUUID().toString())}\n")
        val original=config.readBytes()
        ui.onNodeWithText(alias).performClick(); phase(SSHAuthPhase.HOSTKEY)
        ui.onNodeWithText("信任并连接").performClick(); phase(SSHAuthPhase.CREDENTIALS)
        ui.onNodeWithTag("ssh-key-${key.id}").assertDoesNotExist()
        ui.onNodeWithText("认证并连接").assertIsNotEnabled()
        ui.onNodeWithTag("ssh-other-identity").performScrollTo().performClick()
        ui.onNodeWithTag("ssh-key-${key.id}").performScrollTo().performClick()
        ui.onNodeWithText("认证并连接").performClick()
        await("explicit key authenticated") { sessions.connecting==null && sessions.active?.state?.connected==true }
        assertArrayEquals(original,config.readBytes())
    }
    private fun find(view: View): TerminalView? {
        if(view is TerminalView) return view
        if(view is ViewGroup) for(i in 0 until view.childCount) find(view.getChildAt(i))?.let { return it }
        return null
    }
    private fun terminal()=checkNotNull(find(ui.activity.window.decorView))
    private fun pid(prefix: String): String? = terminal().currentFrame?.let { frame ->
        (0 until frame.rows).map(frame::line).firstOrNull { it.startsWith(prefix) && it.removePrefix(prefix).matches(Regex("[0-9]+")) }
    }
    @Test fun hostEditorAuthenticationRenameAndRemovalKeepActiveRemotePid() {
        editPage(); ui.onNodeWithTag("host-add-identity-${key.id}").performScrollTo().performClick()
        ui.onNodeWithText("保存").performClick()
        await("host association saved") { !hosts.busy && hosts.page==Page.HOSTS }
        assertEquals(listOf(IdentityReferences.encode(key.id)),ConfigBridge.identities(config.readBytes(),alias).map { it.value })
        ui.onNodeWithText(alias).performClick(); phase(SSHAuthPhase.HOSTKEY)
        ui.onNodeWithText("信任并连接").performClick(); phase(SSHAuthPhase.CREDENTIALS)
        ui.onNodeWithTag("ssh-key-${other.id}").assertDoesNotExist()
        ui.onNodeWithTag("ssh-key-${key.id}").performScrollTo().assertTextContains("✓ z-linked",substring=true)
        ui.onNodeWithText("认证并连接").performClick()
        await("configured key authenticated") { sessions.connecting==null && sessions.active?.state?.connected==true }
        await("terminal frame") { find(ui.activity.window.decorView)?.currentFrame!=null }
        val id=main { sessions.active!!.id }
        main { terminal().commitText("stty -echo; PS1=''; printf '\\033[2J\\033[HLINKED:%s\\r\\n' \"\u0024\u0024\"\r") }
        await("remote PID before rename") { pid("LINKED:")!=null }
        val before=main { pid("LINKED:")!! }
        ui.onNodeWithTag("terminal-back").performClick()
        editPage(); ui.onNodeWithText("别名").performScrollTo().performTextReplacement("$alias-new")
        ui.onNodeWithText("保存").performClick()
        await("renamed") { !hosts.busy && hosts.page==Page.HOSTS }
        settingsDetail()
        ui.onNodeWithTag("identity-usage-$alias-new").performScrollTo().assertExists()
        ui.onNodeWithText("移除此身份").performScrollTo().performClick()
        ui.onNodeWithText("确认移除").performClick()
        await("removed") { !identities.busy && identities.selected==null }
        assertEquals(listOf(IdentityReferences.encode(key.id)),ConfigBridge.identities(config.readBytes(),"$alias-new").map { it.value })
        assertEquals(1,main { sessions.sessions.size })
        assertEquals(id,main { sessions.sessions.single().id })
        assertTrue(main { sessions.sessions.single().state.connected })
        main { sessions.show(id) }
        await("return to active terminal") { find(ui.activity.window.decorView)?.currentFrame!=null }
        main { terminal().commitText("printf '\\033[2J\\033[HAFTER:%s\\r\\n' \"\u0024\u0024\"\r") }
        await("remote PID preserved after identity removal") { pid("AFTER:")==before.replace("LINKED:","AFTER:") }
    }
}
