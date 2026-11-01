package com.hanxi.ghosterm

import android.os.SystemClock
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.json.JSONObject
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Real MainActivity/default ViewModel factory and production private-file persistence. */
@RunWith(AndroidJUnit4::class)
class SettingsScreenTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private lateinit var model: SettingsViewModel
    private lateinit var hosts: HostsViewModel
    private lateinit var owner: AppSettings
    private lateinit var file: File
    private var original: ByteArray? = null
    private var backedUp = false
    private fun <T> main(block: () -> T): T {
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) return block()
        var value: T? = null
        instrumentation.runOnMainSync { value = block() }
        @Suppress("UNCHECKED_CAST") return value as T
    }
    private fun await(label: String = "Settings screen did not settle", predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 10_000
        while (SystemClock.elapsedRealtime() < end) { if (main(predicate)) return; SystemClock.sleep(20) }
        fail(label)
    }
    private fun idle() = await { model.inFlight == 0 && owner.pendingCount.value == 0 }
    private fun reload() = runBlocking { withTimeout(10_000) { owner.reload(UUID.randomUUID()) } }
    @Before fun setup() {
        val args = InstrumentationRegistry.getArguments()
        args.getString("settingsFontScale")?.let { expected ->
            assertEquals("Fixture: system font scale did not apply", expected.toFloat(), ui.activity.resources.configuration.fontScale, .01f)
        }
        args.getString("settingsMaxWidthDp")?.let { expected ->
            assertTrue("Fixture: device is wider than the requested narrow screen", ui.activity.resources.configuration.screenWidthDp <= expected.toInt())
        }
        main {
            model = ViewModelProvider(ui.activity)[SettingsViewModel::class.java]
            hosts = ViewModelProvider(ui.activity)[HostsViewModel::class.java]
            owner = (ui.activity.application as GhostermApplication).settings
        }
        await { model.canEdit && !hosts.busy }; idle()
        file = File(ui.activity.filesDir, "preferences/preferences.json")
        original = if (file.exists()) file.readBytes() else null; backedUp = true
        assertTrue(runBlocking { withTimeout(10_000) { owner.save(UUID.randomUUID(), PreferenceEdit.defaults()) } } is PreferenceUpdate.Saved)
        await { model.candidate == AppPreferences() }; idle()
        ui.onNodeWithText("设置").performClick()
    }
    @After fun cleanup() {
        if (!backedUp) return
        idle()
        // Restore only this fixture's preference document, byte for byte, then reread it.
        // Host configs, credentials, trust records, files and sessions are never removed.
        if (original == null) { if (file.exists()) check(file.delete()) }
        else file.writeBytes(checkNotNull(original))
        reload(); idle()
        if (original != null) assertArrayEquals(original, file.readBytes()) else assertFalse(file.exists())
    }
    private fun section(value: SettingsSection) {
        ui.onNodeWithTag("settings-section-${value.name}").performScrollTo().performClick()
        assertEquals(value, main { model.section })
    }
    private fun row(key: PreferenceKey) = ui.onNodeWithTag("settings-row-${key.id}")
    private fun choose(key: PreferenceKey, id: String) {
        row(key).performScrollTo().performClick()
        ui.onNodeWithTag("settings-option-${key.id}-$id").performScrollTo().performClick()
        idle()
    }
    private fun disk(key: String): Any = JSONObject(file.readText()).getJSONObject("values").get(key)

    private fun assertApplicationAppearance(dark: Boolean) {
        ui.waitForIdle()
        // Observe the window buffer and native system-bar controller, not only a model enum.
        val wanted = if (dark) 0xff171b20.toInt() else 0xfff7f8fc.toInt()
        val end = SystemClock.elapsedRealtime() + 10_000
        var matching = 0
        var sampled = 0
        while (SystemClock.elapsedRealtime() < end) {
            ui.waitForIdle()
            val bitmap = checkNotNull(instrumentation.uiAutomation.takeScreenshot())
            matching = 0; sampled = 0
            for (y in bitmap.height / 5 until bitmap.height * 4 / 5 step 8) {
                for (x in listOf(4, bitmap.width - 5)) {
                    sampled++
                    if (bitmap.getPixel(x, y) == wanted) matching++
                }
            }
            if (SystemClock.elapsedRealtime() + 100 >= end) {
                java.io.File(ui.activity.cacheDir, "appearance-failed.png").outputStream().use {
                    bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it)
                }
            }
            bitmap.recycle()
            val bars = main {
                val controller = androidx.core.view.WindowCompat.getInsetsController(ui.activity.window, ui.activity.window.decorView)
                controller.isAppearanceLightStatusBars == !dark && controller.isAppearanceLightNavigationBars == !dark
            }
            if (matching > sampled * 4 / 5 && bars) return
            SystemClock.sleep(50)
        }
        fail("Actual application palette/system bars did not become dark=$dark ($matching/$sampled background pixels)")
    }

    @Test fun applicationAppearanceChangesWindowPixelsBarsAndSurvivesRecreation() {
        section(SettingsSection.APPEARANCE)
        choose(PreferenceKey.APPEARANCE, "light")
        assertApplicationAppearance(false)
        choose(PreferenceKey.APPEARANCE, "dark")
        assertApplicationAppearance(true)
        ui.activityRule.scenario.recreate()
        main { model = ViewModelProvider(ui.activity)[SettingsViewModel::class.java] }
        await { model.section == SettingsSection.APPEARANCE && model.candidate.appearance == AppAppearance.DARK }
        assertApplicationAppearance(true)
        choose(PreferenceKey.APPEARANCE, "light")
        assertApplicationAppearance(false)
    }

    private fun systemNight(command: String = ""): String =
        instrumentation.uiAutomation.executeShellCommand("cmd uimode night" + if (command.isEmpty()) "" else " $command").use {
            java.io.FileInputStream(it.fileDescriptor).bufferedReader().use { reader -> reader.readText().trim() }
        }

    @Test fun systemAppearanceTracksAndroidNightModeInBothDirections() {
        val initial = systemNight().removePrefix("Night mode: ")
        assertTrue("Fixture: unsupported system night setting $initial", initial in setOf("auto", "yes", "no"))
        section(SettingsSection.APPEARANCE)
        choose(PreferenceKey.APPEARANCE, "system")
        try {
            systemNight("yes")
            assertApplicationAppearance(true)
            systemNight("no")
            assertApplicationAppearance(false)
        } finally {
            systemNight(initial)
            assertEquals("Night mode: $initial", systemNight())
        }
    }

    @Test fun resetPreservesLiveShellProtectedFilesIdentityClipboardAndQueuedRequest() {
        val fixture = sshFixture()
        val app = ui.activity.application as GhostermApplication
        val sessions = main { ViewModelProvider(ui.activity)[SessionsViewModel::class.java] }
        val work = app.fileWork
        await { !work.tasks.journalLoading }
        assertFalse("Fixture: another task is active", main { work.tasks.busy })
        val oldClipboard = main { work.clipboard.snapshot }
        val oldHold = main { work.tasks.backgroundHold }
        val config = File(app.filesDir, "home/.ssh/config")
        val known = File(app.filesDir, "home/.ssh/known_hosts")
        val originals = listOf(config, known).associateWith { it.takeIf(File::exists)?.readBytes() }
        val token = UUID.randomUUID().toString()
        val directory = File(app.filesDir, "home/reset-protection-$token").apply { check(mkdirs()) }
        val source = File(directory, "source.txt").apply { writeText("reset keeps this file\n") }
        val vault = com.hanxi.ghosterm.data.IdentityVault(File(app.filesDir, "identities"))
        var identity: com.hanxi.ghosterm.data.StoredIdentity? = null
        var session: TerminalSession? = null
        var task: FileTask? = null
        fun digest(bytes: ByteArray) = java.security.MessageDigest.getInstance("SHA-256").digest(bytes)
        fun identityDigest(): ByteArray {
            val unlocked = vault.unlock(checkNotNull(identity))
            return try { digest(unlocked) } finally { unlocked.fill(0) }
        }
        fun terminalOrNull(): com.hanxi.ghosterm.terminal.TerminalView? = main {
            fun find(view: android.view.View): com.hanxi.ghosterm.terminal.TerminalView? {
                if (view is com.hanxi.ghosterm.terminal.TerminalView) return view
                if (view is android.view.ViewGroup) for (i in 0 until view.childCount)
                    find(view.getChildAt(i))?.let { return it }
                return null
            }
            find(ui.activity.window.decorView)
        }
        fun terminal() = checkNotNull(terminalOrNull())
        fun pid(prefix: String): String? = main {
            session?.surface?.frame(0)?.let { frame ->
                (0 until frame.rows).map(frame::line).firstOrNull { it.matches(Regex("$prefix[0-9]+")) }
            }
        }
        try {
            config.parentFile!!.mkdirs()
            config.writeBytes((originals[config] ?: byteArrayOf()) + "\nHost reset-protection-$token\n HostName reset.example\n".toByteArray())
            val public = fixture.getString("public").trim().split(Regex("\\s+")).take(2).joinToString(" ")
            known.writeBytes((originals[known] ?: byteArrayOf()) + "\nreset.example $public\n".toByteArray())
            val protectedBytes = listOf(config, known, source).associateWith { it.readBytes() }
            identity = vault.import("reset-protection-$token", fixture.getString("pem").toByteArray())
            val keyDigest = identityDigest()
            val envelope = File(app.filesDir, "identities/${checkNotNull(identity).id}.json")
            val envelopeDigest = digest(envelope.readBytes())
            val saved = runBlocking { owner.save(UUID.randomUUID(), listOf(
                PreferenceEdit.Appearance(AppAppearance.DARK), PreferenceEdit.FontSize(18),
                PreferenceEdit.Conflict(com.hanxi.ghosterm.domain.FileConflictPolicy.KEEP_BOTH),
                PreferenceEdit.Clipboard(com.hanxi.ghosterm.domain.ClipboardAfterPaste.CLEAR))) }
            assertTrue(saved is PreferenceUpdate.Saved); idle()
            val customized = owner.state.value.committed
            val location = com.hanxi.ghosterm.domain.FileLocation(com.hanxi.ghosterm.domain.FileEndpoint.APP, directory.name)
            val clipboard = com.hanxi.ghosterm.domain.FileClipboardSnapshot.capture(location, directory.name,
                listOf(com.hanxi.ghosterm.domain.BrowserEntry("${directory.name}/source.txt", "source.txt",
                    com.hanxi.ghosterm.domain.FileKind.FILE, source.length().toULong(), source.lastModified())))
            main {
                work.clipboard.snapshot = clipboard
                work.tasks.holdForBackground("reset protection fixture")
                work.tasks.start(clipboard, location, "same folder", { error("Queued fixture must not open a backend") }, customized)
                task = work.tasks.tasks.first { it.request.clipboardId == clipboard.id }
            }
            await { checkNotNull(task).recorded && !checkNotNull(task).recording }
            assertEquals(FileTaskPhase.QUEUED, main { checkNotNull(task).phase })
            ui.onNodeWithText("返回").performClick()
            ui.onNodeWithText("本地终端").performClick()
            await { sessions.active?.state?.connected == true }
            session = main { checkNotNull(sessions.active) }
            ui.waitForIdle()
            ui.waitUntil(10_000) { terminalOrNull() != null }
            val surface = checkNotNull(session).surface
            terminal().let { view -> main { view.commitText("stty -echo; PS1=''; printf '\\033[3J\\033[2J\\033[HRESET_PID:%s\\r\\n' \"\u0024\u0024\"\r") } }
            await { pid("RESET_PID:") != null }
            val beforePid = checkNotNull(pid("RESET_PID:")).removePrefix("RESET_PID:")
            ui.onNodeWithTag("terminal-back").performClick()
            ui.onNodeWithText("设置").performClick()
            ui.onNodeWithText("恢复默认设置").performScrollTo().performClick()
            ui.onNodeWithText("保留设置").performClick(); idle()
            assertEquals(customized, owner.state.value.committed)
            ui.onNodeWithText("恢复默认设置").performScrollTo().performClick()
            ui.onNodeWithText("恢复默认").performClick(); idle()
            await { owner.state.value.committed == AppPreferences() && model.candidate == AppPreferences() }
            val persisted = runBlocking { PreferencesRepository(PreferencesFile(file.parentFile!!)).reload() }
            assertTrue(persisted is PreferencesState.Ready)
            assertEquals(AppPreferences(), (persisted as PreferencesState.Ready).values)
            protectedBytes.forEach { (path, bytes) ->
                assertTrue("Reset must preserve file ${path.name}", path.isFile)
                assertArrayEquals("Reset must preserve ${path.name}", digest(bytes), digest(path.readBytes()))
            }
            assertTrue("Reset must preserve the encrypted identity envelope", envelope.isFile)
            assertArrayEquals(envelopeDigest, digest(envelope.readBytes()))
            assertArrayEquals(keyDigest, identityDigest())
            main {
                assertSame(clipboard, work.clipboard.snapshot)
                assertTrue(work.tasks.tasks.any { it === task })
                assertEquals(FileTaskPhase.QUEUED, checkNotNull(task).phase)
                assertEquals(com.hanxi.ghosterm.domain.FileConflictPolicy.KEEP_BOTH, checkNotNull(task).request.conflictPolicy)
                assertTrue(checkNotNull(task).clearClipboardAfterSuccess)
                assertFalse(checkNotNull(task).cancelled)
                assertSame(session, sessions.active); assertSame(surface, sessions.active!!.surface)
                assertTrue(sessions.active!!.state.connected)
            }
            ui.onNodeWithText("返回").performClick()
            ui.onNodeWithText("继续本地终端").performClick()
            ui.waitForIdle()
            ui.waitUntil(10_000) { terminalOrNull() != null }
            terminal().let { view -> main { view.commitText("printf '\\033[2J\\033[HRESET_AFTER:%s\\r\\n' \"\u0024\u0024\"\r") } }
            await { pid("RESET_AFTER:") != null }
            assertEquals(beforePid, checkNotNull(pid("RESET_AFTER:")).removePrefix("RESET_AFTER:"))
        } finally {
            session?.let { current -> main { sessions.close(current.id) } }
            task?.let { current ->
                main { work.tasks.cancel(current) }
                await { !current.active && !current.recording }
                main { work.tasks.remove(current) }
                await { current !in work.tasks.tasks }
            }
            main {
                work.clipboard.snapshot = oldClipboard
                if (oldHold == null) work.tasks.allowBackgroundScheduling() else work.tasks.holdForBackground(oldHold)
            }
            identity?.let { current ->
                try { vault.remove(current) } catch (_: java.nio.file.NoSuchFileException) { }
            }
            originals.forEach { (path, bytes) -> if (bytes == null) path.delete() else path.writeBytes(bytes) }
            directory.deleteRecursively()
        }
    }

    @Test fun fileMenuHiddenPreferencePersistsAndSettingsUpdatesTheExistingBrowser() {
        val files = main { ViewModelProvider(ui.activity)[FilesViewModel::class.java] }
        val hidden = File(ui.activity.filesDir, "home/.menu-preference-${UUID.randomUUID()}")
        hidden.parentFile!!.mkdirs(); hidden.writeText("hidden menu fixture\n")
        try {
            ui.onNodeWithText("返回").performClick()
            ui.onNodeWithText("本机文件").performClick()
            ui.waitUntil(10_000) { main { !files.loading && files.entries.any { it.name == hidden.name } } }
            ui.onNodeWithTag("file-entry:${hidden.name}").assertDoesNotExist()
            val entries = main { files.entries }
            ui.onNodeWithText("更多").performClick()
            ui.onNodeWithText("显示隐藏文件").assertIsEnabled().performClick()
            await("Hidden file menu did not commit the preference") { files.showHidden && !files.hiddenSaving && owner.state.value.committed.showHidden }
            assertEquals(true, disk("showHidden")); main { assertSame(entries, files.entries) }
            ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:${hidden.name}"))
            ui.onNodeWithTag("file-entry:${hidden.name}").assertIsDisplayed()
            ui.onNodeWithText("返回").performClick()
            ui.onNodeWithText("设置").performClick()
            section(SettingsSection.FILES)
            row(PreferenceKey.SHOW_HIDDEN).performScrollTo().assertIsOn().performClick(); idle()
            await { !files.showHidden && !owner.state.value.committed.showHidden }
            assertEquals(false, disk("showHidden"))
            ui.onNodeWithText("返回").performClick(); ui.onNodeWithText("返回").performClick()
            ui.onNodeWithText("本机文件").performClick()
            ui.onNodeWithTag("file-entry:${hidden.name}").assertDoesNotExist()
            ui.activityRule.scenario.recreate()
            assertSame(files, main { ViewModelProvider(ui.activity)[FilesViewModel::class.java] })
            ui.onNodeWithTag("file-location").assertIsDisplayed()
            ui.onNodeWithTag("file-entry:${hidden.name}").assertDoesNotExist()
            assertEquals(false, disk("showHidden"))
        } finally { hidden.delete() }
    }

    @Test fun fiveCategoriesHaveOneBackPathAndSecurityRoutes() {
        val firstRows = mapOf(SettingsSection.APPEARANCE to PreferenceKey.APPEARANCE,
            SettingsSection.INPUT to PreferenceKey.AUX_KEYS, SettingsSection.SESSIONS to PreferenceKey.KEEP_AWAKE,
            SettingsSection.FILES to PreferenceKey.SHOW_HIDDEN)
        firstRows.forEach { (category, first) ->
            section(category); row(first).performScrollTo().assertIsDisplayed()
            ui.onNodeWithText("返回").performClick()
            assertNull(main { model.section }); assertEquals(Page.SETTINGS, main { hosts.page })
        }
        section(SettingsSection.SECURITY)
        ui.onNodeWithText("SSH 身份").performScrollTo().assertHasClickAction()
        ui.onNodeWithText("主机验证记录").performScrollTo().performClick()
        await { hosts.page == Page.KNOWN_HOSTS }
        ui.onNodeWithText("返回").performClick()
        assertEquals(Page.SETTINGS, main { hosts.page })
        assertEquals(SettingsSection.SECURITY, main { model.section })
        ui.onNodeWithText("返回").performClick(); ui.onNodeWithText("返回").performClick()
        assertEquals(Page.HOSTS, main { hosts.page })
    }

    @Test fun singleSwitchTapPersistsAndRecreationKeepsCategoryAndValue() {
        section(SettingsSection.INPUT)
        row(PreferenceKey.AUX_KEYS).performScrollTo().assertIsOn().performClick(); idle()
        row(PreferenceKey.AUX_KEYS).assertIsOff()
        assertEquals(false, disk("auxKeys"))
        ui.activityRule.scenario.recreate()
        assertSame(model, main { ViewModelProvider(ui.activity)[SettingsViewModel::class.java] })
        assertEquals(SettingsSection.INPUT, main { model.section })
        row(PreferenceKey.AUX_KEYS).performScrollTo().assertIsOff().performClick(); idle()
        row(PreferenceKey.AUX_KEYS).assertIsOn(); assertEquals(true, disk("auxKeys"))
    }

    @Test fun choicesSliderAndConfirmedResetUseProductionStorage() {
        section(SettingsSection.APPEARANCE)
        choose(PreferenceKey.TERMINAL_THEME, "paper")
        row(PreferenceKey.FONT_SIZE).performScrollTo().performSemanticsAction(SemanticsActions.SetProgress) { it(18f) }
        idle(); assertEquals(18, disk("fontSize")); assertEquals("paper", disk("terminalTheme"))
        choose(PreferenceKey.APPEARANCE, "dark")
        assertEquals(AppAppearance.DARK, main { owner.state.value.committed.appearance })
        ui.onNodeWithText("返回").performClick()
        ui.onNodeWithText("恢复默认设置").performScrollTo().performClick()
        ui.onNodeWithText("保留设置").performClick(); assertEquals(18, disk("fontSize"))
        ui.onNodeWithText("恢复默认设置").performClick(); ui.onNodeWithText("恢复默认").performClick()
        idle(); assertEquals(AppPreferences(), main { owner.state.value.committed })
        assertEquals(12, disk("fontSize")); assertEquals("graphite", disk("terminalTheme"))
    }

    @Test fun damagedDocumentIsVisibleAndDisabledUntilReloadSucceeds() {
        section(SettingsSection.FILES)
        val good = file.readBytes(); val bad = "broken preference fixture".toByteArray()
        idle(); file.writeBytes(bad); reload()
        await { model.storage is PreferencesState.Unavailable }
        ui.onNodeWithText("设置需要重新载入").assertIsDisplayed()
        row(PreferenceKey.SHOW_HIDDEN).performScrollTo().assertIsNotEnabled()
        assertArrayEquals(bad, file.readBytes())
        file.writeBytes(good)
        ui.onNodeWithText("重新载入").performScrollTo().performClick()
        await { model.canEdit }; idle()
        row(PreferenceKey.SHOW_HIDDEN).performScrollTo().assertIsEnabled().assertIsOff()
        assertArrayEquals(good, file.readBytes())
    }

    @Test fun categoryRowsRemainReachableInsideTheScreen() {
        val rows = mapOf(
            SettingsSection.APPEARANCE to listOf(PreferenceKey.APPEARANCE, PreferenceKey.TERMINAL_THEME,
                PreferenceKey.FONT_SIZE, PreferenceKey.LINE_HEIGHT, PreferenceKey.CURSOR, PreferenceKey.BLINK),
            SettingsSection.INPUT to listOf(PreferenceKey.AUX_KEYS, PreferenceKey.KEY_SET, PreferenceKey.CTRL_MODE, PreferenceKey.AUTO_KEYBOARD),
            SettingsSection.SESSIONS to listOf(PreferenceKey.KEEP_AWAKE),
            SettingsSection.FILES to listOf(PreferenceKey.SHOW_HIDDEN, PreferenceKey.SORT_BY, PreferenceKey.FOLDERS_FIRST,
                PreferenceKey.CONFLICT, PreferenceKey.CLIPBOARD_AFTER_PASTE))
        val root = ui.onRoot().fetchSemanticsNode().boundsInRoot
        val density = ui.activity.resources.displayMetrics.density
        rows.forEach { (category, keys) ->
            section(category)
            keys.forEach { key ->
                val node = row(key).performScrollTo().assertIsDisplayed().fetchSemanticsNode()
                val bounds = node.boundsInRoot
                assertTrue("${key.id} extends beyond screen width", bounds.left >= root.left && bounds.right <= root.right)
                val touch = node.touchBoundsInRoot
                assertTrue("${key.id} must retain a 48dp touch target; visual=$bounds, touch=$touch, size=${node.size}",
                    touch.height >= 48 * density - 1)
            }
            ui.onNodeWithText("返回").performClick()
        }
    }
}
