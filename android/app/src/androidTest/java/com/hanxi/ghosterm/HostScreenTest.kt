package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
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
    @Test fun fieldSourcesAndSharedBlockWarningAreVisible() {
        load("User inherited\nHost demo sibling\n  HostName example.test\n  User explicit\n")
        ui.onAllNodesWithText("编辑")[0].performClick()
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
        ui.onNodeWithText("编辑").performClick()
        ui.onNodeWithText("保存").assertIsNotEnabled()
        ui.onNodeWithText("主机地址").assertIsNotEnabled()
        ui.onNodeWithContentDescription("主机地址使用继承值").performScrollTo().assertIsNotEnabled()
        ui.onNodeWithText("此别名出现在 2 个 Host 块中，无法确定修改目标；请在 SSH 配置页编辑。").assertIsDisplayed()
        ui.onNodeWithText("查看与编辑 SSH 配置").performClick()
        ui.onNodeWithText("返回").performClick()
        ui.onNodeWithText("别名").assertExists()
        ui.onNodeWithText("保存").assertIsNotEnabled()
        ui.onNodeWithText("查看与编辑 SSH 配置").performClick()
        ui.onNodeWithText("配置内容").performTextReplacement("Host dup\n  HostName fixed.test\n  User owner\n")
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy && model.page == Page.EDIT_HOST }
        ui.onNodeWithText("保存").assertIsEnabled()
        ui.onNodeWithText("fixed.test").assertExists()
        ui.onNodeWithText("远端用户名").performScrollTo().assertTextContains("owner")
        assertEquals(1, model.draft!!.original!!.source!!.matchingBlocks)
        ui.onNodeWithText("返回").performClick()
        ui.onNodeWithText("dup").assertIsDisplayed()
    }
    @Test fun inheritanceSwitchSavesSameValueAndThenRemovesOverride() {
        val original = "Host demo\n  HostName example.test\nHost *\n  User inherited\n"
        load(original)
        ui.onNodeWithText("编辑").performClick()
        val toggle = ui.onNodeWithContentDescription("远端用户名使用继承值")
        toggle.performScrollTo().assertIsOn().performClick()
        ui.onNodeWithText("远端用户名").assertIsEnabled().assertTextContains("inherited")
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy && model.page == Page.HOSTS }
        assertEquals("inherited", model.snapshot!!.hosts.single().source!!.user.explicitValue)
        ui.onNodeWithText("编辑").performClick()
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
        for ((label, _) in fields) ui.onNodeWithContentDescription("${label}使用继承值").performScrollTo().performClick()
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy && model.page == Page.HOSTS }
        assertEquals("inherit-all:22", model.snapshot!!.hosts.single().address)
        val original = File(directory, "home/.ssh/config").readText()
        ui.onNodeWithText("编辑").performClick()
        for ((label, value) in fields) {
            ui.onNodeWithContentDescription("${label}使用继承值").performScrollTo().assertIsOn().performClick()
            ui.onNodeWithText(label).performScrollTo().performTextReplacement(value)
        }
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy && model.page == Page.HOSTS }
        assertEquals("owner@example.test:2200", model.snapshot!!.hosts.single().address)
        ui.onNodeWithText("编辑").performClick()
        for ((label, _) in fields) ui.onNodeWithContentDescription("${label}使用继承值").performScrollTo().assertIsOff().performClick()
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy }
        assertEquals("inherit-all:22", model.snapshot!!.hosts.single().address)
        assertEquals(original, File(directory, "home/.ssh/config").readText())
    }
    @Test fun inheritanceChangesRequireDiscardAndRevertingIsClean() {
        val original = "Host demo\n  HostName example.test\n  User alice\n"
        load(original)
        ui.onNodeWithText("编辑").performClick()
        val toggle = ui.onNodeWithContentDescription("远端用户名使用继承值")
        toggle.performScrollTo().performClick()
        ui.onNodeWithText("返回").performClick()
        ui.onNodeWithText("放弃未保存的修改？").assertIsDisplayed()
        ui.onNodeWithText("继续编辑").performClick()
        toggle.assertIsOn().performClick()
        ui.onNodeWithText("远端用户名").assertTextContains("alice")
        ui.onNodeWithText("返回").performClick()
        ui.onNodeWithText("添加主机").assertIsDisplayed()
        ui.onNodeWithText("编辑").performClick()
        toggle.performScrollTo().performClick()
        ui.onNodeWithText("返回").performClick()
        ui.onNodeWithText("放弃修改").performClick()
        ui.onNodeWithText("添加主机").assertIsDisplayed()
        assertEquals(original, File(directory, "home/.ssh/config").readText())
    }
    @Test fun invalidExplicitValueKeepsDraftAndCanReturnToInheritance() {
        val original = "Host demo\n  HostName example.test\n"
        load(original)
        ui.onNodeWithText("编辑").performClick()
        ui.onNodeWithContentDescription("远端用户名使用继承值").performScrollTo().performClick()
        ui.onNodeWithText("远端用户名").performScrollTo().performTextReplacement("bad user")
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { !model.busy }
        assertEquals("Invalid field must keep the editing page", Page.EDIT_HOST, model.page)
        ui.onNodeWithText("请填写远端用户名，或选择使用继承值").assertIsDisplayed()
        ui.onNodeWithText("远端用户名").performScrollTo().assertTextContains("bad user")
        assertEquals(Page.EDIT_HOST, model.page)
        assertEquals(original, File(directory, "home/.ssh/config").readText())
        ui.onNodeWithContentDescription("远端用户名使用继承值").performScrollTo().performClick()
        ui.onNodeWithText("返回").performClick()
        ui.onNodeWithText("添加主机").assertIsDisplayed()
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
        ui.onNodeWithText("搜索名称或地址").performTextInput("no-match")
        ui.onNodeWithText("没有匹配的主机").assertIsDisplayed()
        ui.onNodeWithText("搜索名称或地址").performTextClearance()
        ui.onNodeWithText("编辑").performClick()
        ui.onNodeWithText("alice").assertExists()
        ui.onNodeWithText("远端用户名").performScrollTo().performTextReplacement("bob")
        ui.onNodeWithText("返回").performClick()
        ui.onNodeWithText("放弃未保存的修改？").assertIsDisplayed()
        ui.onNodeWithText("继续编辑").performClick()
        ui.onNodeWithText("保存").performClick()
        ui.waitUntil(10_000) { model.page == Page.HOSTS && !model.busy }
        assertTrue(file.readText().contains("User bob"))
        ui.onNodeWithText("设置").performClick()
        ui.onNodeWithText("查看与编辑 SSH 配置").performClick()
        ui.onNodeWithText("返回").performClick()
        ui.onNodeWithText("关于 Ghosterm").assertIsDisplayed()
    }
}
