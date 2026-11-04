package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.test.espresso.Espresso.closeSoftKeyboard
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class HostScreenTest {
    @get:Rule val ui = createComposeRule()
    private lateinit var directory: File
    private lateinit var model: HostsViewModel
    @Before fun setup() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        directory = File(context.cacheDir, "host-ui-${UUID.randomUUID()}").apply { mkdirs() }
        val application = object : Application() { override fun getFilesDir(): File = directory }
        model = HostsViewModel(application)
        val runtime = RuntimeInfoViewModel()
        ui.setContent { GhostermApp(model, runtime) }
        ui.waitUntil(10_000) { model.snapshot != null && !model.busy }
    }
    @After fun cleanup() { directory.deleteRecursively() }
    private fun load(text: String) {
        File(directory, "home/.ssh/config").writeText(text)
        ui.runOnIdle { model.reload() }
        ui.waitUntil(10_000) { !model.busy && model.snapshot?.text == text }
    }
    private fun openConnectionCategory() {
        // Settings returns to its category; the read-only host editor also links directly to config.
        if (ui.onAllNodesWithTag("settings-section-SESSIONS").fetchSemanticsNodes().isNotEmpty())
            ui.onNodeWithTag("settings-section-SESSIONS").performScrollTo().performClick()
    }
    @Test fun deleteHostRequiresConfirmationAndPreservesSharedAliasAndBackup() {
        val original = "# 保留\r\nHost \"demo\" sibling # shared\r\n User alice\r\nHost other\n Port 2200\n"
        load(original)
        fun request() {
            ui.onNodeWithTag("host-menu:demo").performScrollTo().performClick()
            ui.onNodeWithTag("host-delete:demo").performScrollTo().performClick()
        }
        request()
        ui.onNodeWithText("取消").performClick()
        assertEquals(original, File(directory, "home/.ssh/config").readText())
        request()
        ui.onNodeWithText("共用配置及其他别名保留", substring = true).assertIsDisplayed()
        ui.onNodeWithTag("host-delete-confirm").performClick()
        ui.waitUntil(10_000) { !model.busy && model.snapshot?.hosts?.none { it.alias == "demo" } == true }
        ui.onNodeWithText("sibling").assertIsDisplayed()
        assertEquals(original.replace("\"demo\" ", ""), File(directory, "home/.ssh/config").readText())
        assertEquals(original, File(directory, "home/.ssh").listFiles()!!.single { it.name.startsWith("config.backup-") }.readText())
    }

    @Test fun changedFileWhileDeleteConfirmationIsOpenIsNotOverwritten() {
        val original = "Host demo\n User old\n"
        load(original)
        ui.onNodeWithTag("host-menu:demo").performClick()
        ui.onNodeWithTag("host-delete:demo").performScrollTo().performClick()
        val external = original + "# edited elsewhere\n"
        File(directory, "home/.ssh/config").writeText(external)
        ui.onNodeWithTag("host-delete-confirm").performClick()
        ui.waitUntil(10_000) { !model.busy && model.error != null }
        ui.onNodeWithText("SSH 配置已改变，请重新载入后再编辑").assertIsDisplayed()
        assertEquals(external, File(directory, "home/.ssh/config").readText())
    }

    @Test fun duplicateHostDeletionIsDisabledWithAnExplanation() {
        load("Host demo\n User a\nHost DEMO\n User b\n")
        ui.onNodeWithTag("host-menu:demo").performClick()
        ui.onNodeWithTag("host-delete:demo").performScrollTo().assertIsNotEnabled()
        ui.onNodeWithText("此别名出现在 2 个 Host 块中", substring = true).assertExists()
        ui.onNodeWithTag("host-delete-confirm").assertDoesNotExist()
    }
    @Test fun configEditorDisablesAutocorrectAndWritesLiteralText() {
        load("Host original\n  HostName original.test\n")
        ui.onNode(hasText("设置") or hasContentDescription("设置")).performClick()
        openConnectionCategory()
        ui.onNodeWithText("查看与编辑 SSH 配置").performScrollTo().performClick()
        ui.onNodeWithText("配置内容").performClick()
        ui.waitUntil(10_000) {
            ui.runOnIdle {
                androidx.test.runner.lifecycle.ActivityLifecycleMonitorRegistry.getInstance()
                    .getActivitiesInStage(androidx.test.runner.lifecycle.Stage.RESUMED)
                    .any { it.window.decorView.findFocus()?.onCheckIsTextEditor() == true }
            }
        }
        ui.runOnIdle {
            val focus = androidx.test.runner.lifecycle.ActivityLifecycleMonitorRegistry.getInstance()
                .getActivitiesInStage(androidx.test.runner.lifecycle.Stage.RESUMED)
                .mapNotNull { it.window.decorView.findFocus() }.single { it.onCheckIsTextEditor() }
            val info = android.view.inputmethod.EditorInfo()
            val connection = checkNotNull(focus.onCreateInputConnection(info))
            try {
                assertEquals("SSH config must not request automatic correction", 0,
                    info.inputType and android.text.InputType.TYPE_TEXT_FLAG_AUTO_CORRECT)
                assertEquals("SSH config must request literal input", android.text.InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD,
                    info.inputType and android.text.InputType.TYPE_MASK_VARIATION)
                assertTrue("SSH config must disable input-method suggestions",
                    info.inputType and android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS != 0)
                assertTrue("SSH config editor must retain multiline input",
                    info.inputType and android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE != 0)
            } finally { connection.closeConnection() }
        }
        closeSoftKeyboard()
        val literal = "# 中文注释 ☃\nHost literal\n  HostName ssh.github.com\n  User git\n  Port 22\n"
        ui.onNodeWithText("配置内容").performTextReplacement(literal)
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy && model.page != Page.CONFIG }
        assertEquals("Literal SSH config text must reach disk unchanged", literal,
            File(directory, "home/.ssh/config").readText())
    }
    @Test fun hostClickReportsUnsupportedConfigurationAndLeavesEditorAvailable() {
        val text = "Host gateway-target\n HostName 127.0.0.1\n ProxyJump gateway\n"
        load(text)
        ui.onNodeWithText("gateway-target").performClick()
        ui.waitUntil(10_000) { !model.busy }
        assertEquals("不支持 SSH 配置：proxyjump（当前配置:3）", model.error)
        ui.onNodeWithText("不支持 SSH 配置：proxyjump（当前配置:3）").assertIsDisplayed()
        assertEquals(text, File(directory, "home/.ssh/config").readText())
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        ui.onNodeWithText("主机地址").assertTextContains("127.0.0.1")
    }
    @Test fun invalidAliasAddressAndPortPreserveDraftAndOriginal() {
        val original = "Host original\n  HostName original.test\n  User owner\n  Port 22\n"
        load(original)
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        val cases = listOf(
            Triple("别名", listOf("", "*bad", "a".repeat(129)), "别名只能包含字母、数字、点、短横线与下划线"),
            Triple("主机地址", listOf("", "bad host", "host#comment"), "请填写有效的主机地址，或选择使用继承值"),
            Triple("端口", listOf("0", "65536", "not-a-port"), "端口必须为 1–65535，或选择使用继承值"),
        )
        for ((label, invalid, message) in cases) {
            for (value in invalid) {
                ui.onNodeWithText(label).performScrollTo().performTextReplacement(value)
                closeSoftKeyboard()
                ui.onNodeWithText("保存").performClick()
                ui.waitUntil(10_000) { !model.busy }
                ui.onNodeWithText(message).assertIsDisplayed()
                assertEquals(Page.EDIT_HOST, model.page)
                ui.onNodeWithText(label).performScrollTo().assertTextContains(value)
                assertEquals(original, File(directory, "home/.ssh/config").readText())
                assertTrue(File(directory, "home/.ssh").listFiles()!!.none { it.name.startsWith("config.backup-") })
            }
            val previous = when (label) { "别名" -> "original"; "主机地址" -> "original.test"; else -> "22" }
            ui.onNodeWithText(label).performTextReplacement(previous)
            closeSoftKeyboard()
        }
        assertFalse(model.dirty)
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNodeWithText("放弃未保存的修改？").assertDoesNotExist()
        ui.onNodeWithText("original").assertIsDisplayed()
    }
    @Test fun loadFailureHidesEmptyAndStaleListsAndCanRetry() {
        ui.onNodeWithText("添加你的第一台主机").assertIsDisplayed()
        val file = File(directory, "home/.ssh/config")
        assertTrue(file.mkdir())
        ui.runOnIdle { model.reload() }
        ui.waitUntil(10_000) { !model.busy }
        ui.onNodeWithText("配置不是普通文件或超过 1 MiB").assertIsDisplayed()
        ui.onNodeWithText("添加你的第一台主机").assertDoesNotExist()
        ui.onNodeWithText("添加主机").assertIsNotEnabled()
        ui.onNode(hasText("设置") or hasContentDescription("设置")).performClick()
        ui.onNodeWithText("关于 Ghosterm").performScrollTo().performClick()
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        openConnectionCategory()
        ui.onNodeWithText("管理主机").performScrollTo().performClick()
        ui.onNodeWithText("配置不是普通文件或超过 1 MiB").assertIsDisplayed()
        assertTrue(file.delete())
        file.writeText("Host recovered\n  User owner\n")
        ui.onNodeWithText("重新载入").performClick()
        ui.waitUntil(10_000) { !model.busy }
        ui.onNodeWithText("recovered").assertIsDisplayed()
        ui.onNodeWithText("添加主机").assertIsEnabled()
        assertTrue(file.delete()); assertTrue(file.mkdir())
        ui.runOnIdle { model.reload() }
        ui.waitUntil(10_000) { !model.busy }
        ui.onNodeWithText("recovered").assertDoesNotExist()
        ui.onNodeWithText("添加你的第一台主机").assertDoesNotExist()
    }
    @Test fun invalidConfigurationRemainsErrorUntilReloaded() {
        val file = File(directory, "home/.ssh/config")
        file.writeBytes(byteArrayOf(0xc3.toByte(), 0x28))
        ui.runOnIdle { model.reload() }
        ui.waitUntil(10_000) { !model.busy }
        ui.onNodeWithText("SSH 配置必须为 UTF-8 文本").assertIsDisplayed()
        ui.onNodeWithText("添加你的第一台主机").assertDoesNotExist()
        file.writeText("Include ${directory.path}/missing.conf\n")
        ui.onNodeWithText("重新载入").performClick()
        ui.waitUntil(10_000) { !model.busy }
        ui.onNodeWithText("Configuration parse failed").assertIsDisplayed()
        ui.onNodeWithText("添加主机").assertIsNotEnabled()
        assertTrue(file.delete())
        ui.onNodeWithText("重新载入").performClick()
        ui.waitUntil(10_000) { !model.busy }
        ui.onNodeWithText("添加你的第一台主机").assertIsDisplayed()
        assertNull(model.snapshot!!.revision.identity)
    }
    @Test fun configReloadConfirmsDiscardAndKeepsDraftOnReadFailure() {
        load("Host original\n")
        val file = File(directory, "home/.ssh/config")
        ui.onNode(hasText("设置") or hasContentDescription("设置")).performClick()
        openConnectionCategory()
        ui.onNodeWithText("查看与编辑 SSH 配置").performScrollTo().performClick()
        ui.onNodeWithText("配置内容").performTextReplacement("Host draft\n")
        closeSoftKeyboard()
        file.writeText("Host external\n")
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy }
        ui.onNodeWithText("配置内容").assertTextContains("Host draft\n")
        ui.onNodeWithText("保存").assertIsNotEnabled()
        ui.onNodeWithText("重新载入").performClick()
        ui.onNodeWithText("放弃未保存的修改？").assertIsDisplayed()
        ui.onNodeWithText("继续编辑").performClick()
        ui.onNodeWithText("配置内容").assertTextContains("Host draft\n")
        assertEquals("Host external\n", file.readText())
        assertTrue(file.delete()); assertTrue(file.mkdir())
        ui.onNodeWithText("重新载入").performClick()
        ui.onNodeWithText("放弃修改").performClick()
        ui.waitUntil(10_000) { !model.busy }
        assertEquals(Page.CONFIG, model.page)
        assertNull(model.snapshot)
        ui.onNodeWithText("配置内容").assertTextContains("Host draft\n")
        ui.onNodeWithText("保存").assertIsNotEnabled()
        assertTrue(file.delete()); file.writeText("Host recovered\n")
        ui.onNodeWithText("重新载入").performClick()
        ui.onNodeWithText("放弃修改").performClick()
        ui.waitUntil(10_000) { !model.busy }
        assertEquals(Page.CONFIG, model.page)
        ui.onNodeWithText("配置内容").assertTextContains("Host recovered\n")
        ui.onNodeWithText("保存").assertIsNotEnabled()
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        openConnectionCategory()
        ui.onNodeWithText("管理主机").performScrollTo().performClick()
        ui.onNodeWithText("recovered").assertIsDisplayed()
    }
    @Test fun hostConflictCanReloadOrDiscardDeletedTarget() {
        load("Host demo\n  HostName original.test\n  User owner\n")
        val file = File(directory, "home/.ssh/config")
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        ui.onNodeWithText("主机地址").performTextReplacement("draft.test")
        closeSoftKeyboard()
        file.writeText("Host demo\n  HostName external.test\n  User owner\n")
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy }
        ui.onNodeWithText("主机地址").assertTextContains("draft.test")
        ui.onNodeWithText("重新载入").performClick()
        ui.onNodeWithText("继续编辑").performClick()
        ui.onNodeWithText("主机地址").assertTextContains("draft.test")
        ui.onNodeWithText("重新载入").performClick()
        assertTrue(file.delete()); assertTrue(file.mkdir())
        ui.onNodeWithText("放弃修改").performClick()
        ui.waitUntil(10_000) { !model.busy }
        ui.onNodeWithText("保存").assertIsNotEnabled()
        ui.onNodeWithText("主机地址").assertIsNotEnabled()
        ui.onNodeWithText("查看与编辑 SSH 配置").assertIsNotEnabled()
        assertTrue(file.delete())
        file.writeText("Host demo\n  HostName external.test\n  User owner\n")
        ui.onNodeWithText("重新载入").performClick()
        ui.onNodeWithText("放弃修改").performClick()
        ui.waitUntil(10_000) { !model.busy }
        assertEquals(Page.EDIT_HOST, model.page)
        ui.onNodeWithText("主机地址").assertTextContains("external.test")
        ui.onNodeWithText("主机地址").performTextReplacement("next-draft.test")
        closeSoftKeyboard()
        file.writeText("Host other\n")
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy }
        ui.onNodeWithText("重新载入").performClick()
        ui.onNodeWithText("放弃修改").performClick()
        ui.waitUntil(10_000) { !model.busy }
        assertEquals(Page.HOSTS, model.page)
        ui.onNodeWithText("other").assertIsDisplayed()
        ui.onNodeWithText("demo").assertDoesNotExist()
        assertFalse(File(directory, "home/.ssh").listFiles()!!.any { it.name.startsWith("config.backup-") })
    }
    @Test fun fieldSourcesAndSharedBlockWarningAreVisible() {
        load("User inherited\nHost demo sibling\n  HostName example.test\n  User explicit\n")
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        ui.onNodeWithText("Host demo sibling").assertIsDisplayed()
        ui.onNodeWithText("此 Host 块包含多个别名或规则；修改会同时影响上方列出的全部名称。").assertIsDisplayed()
        ui.onNodeWithText("继承自全局配置 · 第 1 行；此块设置的 explicit 未生效").performScrollTo().assertIsDisplayed()
        ui.onNodeWithText("当前生效：inherited").performScrollTo().assertIsDisplayed()
        ui.onNodeWithText("远端用户名").assertTextContains("explicit")
        ui.onNodeWithText("默认值（未显式设置）").performScrollTo().assertIsDisplayed()
        ui.onNodeWithText("保存").assertIsEnabled()
        ui.onNodeWithText("主机地址").performScrollTo().performTextReplacement("new.test")
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy && model.page == Page.HOSTS }
        assertTrue(model.snapshot!!.hosts.all { it.hostname == "new.test" && it.user == "inherited" })
        assertTrue(File(directory, "home/.ssh/config").readText().contains("  User explicit\n"))
    }
    @Test fun readOnlyHostCanInspectConfigAndReturnWithFreshOwnership() {
        load("Host dup\n  User first\nHost DUP\n  User second\n")
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        ui.onNodeWithText("保存").assertIsNotEnabled()
        ui.onNodeWithText("别名").assertIsNotEnabled()
        ui.onNodeWithText("主机地址").assertIsNotEnabled()
        ui.onNodeWithContentDescription("主机地址使用继承值").performScrollTo().assertIsNotEnabled()
        ui.onNodeWithText("此别名出现在 2 个 Host 块中，无法确定修改目标；请在 SSH 配置页编辑。").assertIsDisplayed()
        openConnectionCategory()
        ui.onNodeWithText("查看与编辑 SSH 配置").performScrollTo().performClick()
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNodeWithText("别名").assertExists()
        ui.onNodeWithText("保存").assertIsNotEnabled()
        openConnectionCategory()
        ui.onNodeWithText("查看与编辑 SSH 配置").performScrollTo().performClick()
        ui.onNodeWithText("配置内容").performTextReplacement("Host dup\n  HostName fixed.test\n  User owner\n")
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy && model.page == Page.EDIT_HOST }
        ui.onNodeWithText("保存").assertIsEnabled()
        ui.onNodeWithText("fixed.test").assertExists()
        ui.onNodeWithText("远端用户名").performScrollTo().assertTextContains("owner")
        assertEquals(1, model.draft!!.original!!.source!!.matchingBlocks)
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNodeWithText("dup").assertIsDisplayed()
    }
    @Test fun inheritanceSwitchSavesSameValueAndThenRemovesOverride() {
        val original = "Host demo\n  HostName example.test\nHost *\n  User inherited\n"
        load(original)
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        val toggle = ui.onNodeWithContentDescription("远端用户名使用继承值")
        toggle.performScrollTo().assertIsOn().performClick()
        ui.onNodeWithText("远端用户名").assertIsEnabled().assertTextContains("inherited")
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy && model.page == Page.HOSTS }
        assertEquals("inherited", model.snapshot!!.hosts.single().source!!.user.explicitValue)
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        toggle.performScrollTo().assertIsOff().performClick()
        ui.onNodeWithText("远端用户名").assertIsNotEnabled()
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy && model.page == Page.HOSTS }
        assertEquals(original, File(directory, "home/.ssh/config").readText())
        assertNull(model.snapshot!!.hosts.single().source!!.user.explicitValue)
    }
    @Test fun allFieldsCanInheritForNewAndExistingHost() {
        ui.onNodeWithText("添加主机").performClick()
        ui.onNodeWithText("别名").performTextInput("inherit-all")
        val fields = listOf("主机地址" to "example.test", "远端用户名" to "owner", "端口" to "2200")
        // Finish the IME transition before scrolling to the next touch target.
        // Otherwise bring-into-view can move the switch after coordinates are sampled.
        closeSoftKeyboard()
        for ((label, _) in fields) ui.onNodeWithContentDescription("${label}使用继承值").performScrollTo().performClick().assertIsOn()
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy && model.page == Page.HOSTS }
        assertEquals("inherit-all:22", model.snapshot!!.hosts.single().address)
        val original = File(directory, "home/.ssh/config").readText()
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        for ((label, value) in fields) {
            ui.onNodeWithContentDescription("${label}使用继承值").performScrollTo().assertIsOn().performClick()
                .assertIsOff()
            ui.onNodeWithText(label).performScrollTo().performTextReplacement(value)
            closeSoftKeyboard()
        }
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy && model.page == Page.HOSTS }
        assertEquals("owner@example.test:2200", model.snapshot!!.hosts.single().address)
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        for ((label, _) in fields) ui.onNodeWithContentDescription("${label}使用继承值").performScrollTo().assertIsOff().performClick().assertIsOn()
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy }
        assertEquals("inherit-all:22", model.snapshot!!.hosts.single().address)
        assertEquals(original, File(directory, "home/.ssh/config").readText())
    }
    @Test fun inheritanceChangesRequireDiscardAndRevertingIsClean() {
        val original = "Host demo\n  HostName example.test\n  User alice\n"
        load(original)
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        val toggle = ui.onNodeWithContentDescription("远端用户名使用继承值")
        toggle.performScrollTo().performClick()
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNodeWithText("放弃未保存的修改？").assertIsDisplayed()
        ui.onNodeWithText("继续编辑").performClick()
        toggle.assertIsOn().performClick()
        ui.onNodeWithText("远端用户名").assertTextContains("alice")
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNodeWithText("添加主机").assertIsDisplayed()
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        toggle.performScrollTo().performClick()
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNodeWithText("放弃修改").performClick()
        ui.onNodeWithText("添加主机").assertIsDisplayed()
        assertEquals(original, File(directory, "home/.ssh/config").readText())
    }
    @Test fun invalidExplicitValueKeepsDraftAndCanReturnToInheritance() {
        val original = "Host demo\n  HostName example.test\n"
        load(original)
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        ui.onNodeWithContentDescription("远端用户名使用继承值").performScrollTo().performClick()
        ui.onNodeWithText("远端用户名").performScrollTo().performClick()
        ui.waitUntil(10_000) {
            InstrumentationRegistry.getInstrumentation().uiAutomation.executeShellCommand("dumpsys input_method").use {
                java.io.FileInputStream(it.fileDescriptor).bufferedReader().use { reader -> "mInputShown=true" in reader.readText() }
            }
        }
        ui.onNodeWithText("远端用户名").performTextReplacement("bad user")
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy }
        assertEquals("Invalid field must keep the editing page", Page.EDIT_HOST, model.page)
        ui.onNodeWithText("请填写远端用户名，或选择使用继承值").assertIsDisplayed()
        ui.onNodeWithText("远端用户名").performScrollTo().assertTextContains("bad user")
        assertEquals(Page.EDIT_HOST, model.page)
        assertEquals(original, File(directory, "home/.ssh/config").readText())
        ui.onNodeWithContentDescription("远端用户名使用继承值").performScrollTo().performClick()
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNodeWithText("添加主机").assertIsDisplayed()
    }
    @Test fun renameUsesRealFormAndCollisionKeepsDraft() {
        val original = "Host demo sibling\n  HostName example.test\n  User me\nHost occupied\n"
        load(original)
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        ui.onNodeWithText("别名").performScrollTo().assertIsEnabled().performTextReplacement("OCCUPIED")
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy }
        assertEquals(Page.EDIT_HOST, model.page)
        ui.onNodeWithText("该主机别名已存在").assertIsDisplayed()
        ui.onNodeWithText("别名").assertTextContains("OCCUPIED").performTextReplacement("renamed")
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNodeWithText("放弃未保存的修改？").assertIsDisplayed()
        ui.onNodeWithText("继续编辑").performClick()
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy }
        assertEquals(Page.HOSTS, model.page)
        ui.onNodeWithText("renamed").assertIsDisplayed()
        ui.onNodeWithText("sibling").assertIsDisplayed()
        assertEquals(original.replace("Host demo", "Host renamed"), File(directory, "home/.ssh/config").readText())
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        ui.waitUntil(10_000) { model.page==Page.EDIT_HOST && model.draft!=null }
        ui.onNodeWithText("别名").performScrollTo().performTextReplacement("discarded")
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNodeWithText("放弃修改").performClick()
        ui.onNodeWithText("renamed").assertIsDisplayed()
        assertEquals(listOf("renamed", "sibling", "occupied"), model.snapshot!!.hosts.map { it.alias })
    }
    @Test fun addEditSearchAndDiscardUseRealConfiguration() {
        ui.onNodeWithText("添加主机").performClick()
        ui.onNodeWithText("别名").performTextInput("ui-host")
        ui.onNodeWithText("主机地址").performScrollTo().performTextInput("192.0.2.8")
        ui.onNodeWithText("远端用户名").performScrollTo().performTextInput("alice")
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { model.page == Page.HOSTS && !model.busy }
        ui.onNodeWithText("ui-host").assertIsDisplayed()
        val file = File(directory, "home/.ssh/config")
        assertTrue(file.readText().contains("Host ui-host"))
        ui.onNodeWithTag("hosts-search").performTextInput("no-match")
        ui.onNodeWithText("没有找到主机").assertIsDisplayed()
        ui.onNodeWithTag("hosts-search").performTextClearance()
        ui.onAllNodesWithContentDescription("主机操作").onFirst().performScrollTo().performClick();ui.onNodeWithText("编辑主机").performClick()
        ui.onNodeWithText("alice").assertExists()
        ui.onNodeWithText("远端用户名").performScrollTo().performTextReplacement("bob")
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNodeWithText("放弃未保存的修改？").assertIsDisplayed()
        ui.onNodeWithText("继续编辑").performClick()
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { model.page == Page.HOSTS && !model.busy }
        assertTrue(file.readText().contains("User bob"))
        ui.onNode(hasText("设置") or hasContentDescription("设置")).performClick()
        openConnectionCategory()
        ui.onNodeWithText("查看与编辑 SSH 配置").performScrollTo().performClick()
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNodeWithText("关于 Ghosterm").performScrollTo().assertIsDisplayed()
    }
}
