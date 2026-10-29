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
    @Test fun addEditSearchAndDiscardUseRealConfiguration() {
        ui.onNodeWithText("添加主机").performClick()
        ui.onNodeWithText("别名").performTextInput("ui-host")
        ui.onNodeWithText("主机地址").performTextInput("192.0.2.8")
        ui.onNodeWithText("远端用户名").performTextInput("alice")
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
        ui.onNodeWithText("远端用户名").performTextReplacement("bob")
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
