package com.hanxi.ghosterm

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelStore
import com.hanxi.ghosterm.bridge.PreparedSSH
import kotlinx.coroutines.awaitCancellation
import org.junit.After
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test

class FileConnectionPresentationTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private val store = ViewModelStore()
    @After fun cleanup() { ui.runOnIdle { store.clear() } }
    private fun setup(user: String): SessionsViewModel {
        val application=ui.activity.application
        val model=SessionsViewModel(application, SSHResolver { awaitCancellation() }, monitorSessions=false)
        val browser=FilesViewModel(application)
        ui.runOnUiThread {
            store.put("connection",model);store.put("browser",browser)
            browser.enterLocal()
            model.connectFiles(PreparedSSH("example","example.invalid",user,22,emptyList())) { connection,_ -> connection.close() }
        }
        ui.setContent { MaterialTheme {
            SSHConnectionDialog(model)
            FilesScreen(browser, {}, connectionProgress=model.connecting?.takeIf { it.passiveProgress }?.message,
                cancelConnection={ model.connecting?.let { model.cancelConnection(it.id) } })
        } }
        return model
    }
    @Test fun passiveStagesShareOnePageIndicatorAndCanBeCancelled() {
        val model=setup("user")
        ui.onNodeWithTag("file-connection-progress").assertIsDisplayed()
        ui.onAllNodes(isDialog()).assertCountEquals(0)
        val attempt=model.connecting!!.id
        ui.runOnIdle { model.connecting!!.message="正在打开远端文件…" }
        ui.onNodeWithTag("file-connection-progress").assertIsDisplayed()
        ui.onNodeWithText("正在打开远端文件…").assertIsDisplayed()
        ui.onAllNodes(isDialog()).assertCountEquals(0)
        assertEquals(attempt,model.connecting!!.id)
        ui.onNodeWithText("取消").performClick()
        ui.onNodeWithTag("file-connection-progress").assertDoesNotExist()
        ui.runOnIdle { assertNull(model.connecting) }
    }
    @Test fun missingUsernameStillRequiresAnExplicitDialog() {
        setup("")
        ui.onNodeWithTag("ssh-username").assertIsDisplayed()
        ui.onAllNodes(isDialog()).assertCountEquals(1)
        ui.onNodeWithTag("file-connection-progress").assertDoesNotExist()
    }
    private fun setupTask(): SessionsViewModel {
        val model=SessionsViewModel(ui.activity.application, SSHResolver { awaitCancellation() }, monitorSessions=false)
        val foreground=SessionsViewModel(ui.activity.application, SSHResolver { awaitCancellation() }, monitorSessions=false)
        ui.runOnUiThread {
            store.put("task",model);store.put("foreground",foreground)
            model.connect(PreparedSSH("copy-host","example.invalid","user",22,emptyList()))
            model.connecting!!.taskLabel="复制任务 · 读取来源"
        }
        ui.setContent { MaterialTheme {
            SSHConnectionLayout(foreground,model) { SSHConnectionDialog(model) }
        } }
        return model
    }
    @Test fun copyRolesUseInlineProgressAndCancelTheTask() {
        val model=setupTask()
        var cancelled=false
        ui.runOnIdle { model.connecting!!.taskCancelled={ cancelled=true } }
        for(role in listOf("读取来源","写入目标")) {
            for(message in listOf("正在连接 SSH…","正在认证…","正在打开远端文件…")) {
                ui.runOnIdle {
                    model.connecting!!.taskLabel="复制任务 · $role"
                    model.connecting!!.message=message
                }
                ui.onNodeWithTag("ssh-connection-progress").assertIsDisplayed()
                ui.onNodeWithText("复制任务 · $role").assertIsDisplayed()
                ui.onNodeWithText(message).assertIsDisplayed()
                ui.onAllNodes(isDialog()).assertCountEquals(0)
            }
        }
        ui.onNodeWithTag("ssh-connection-cancel").performClick()
        ui.runOnIdle { assertTrue(cancelled);assertNull(model.connecting) }
        ui.onNodeWithTag("ssh-connection-progress").assertDoesNotExist()
    }
    @Test fun copyFailureStillRequiresExplicitDismissal() {
        val model=setupTask()
        ui.runOnIdle { model.connecting!!.let { it.message="任务连接失败";it.failed=true } }
        ui.onNodeWithTag("ssh-connection-progress").assertDoesNotExist()
        ui.onNodeWithTag("ssh-failure").assertTextEquals("任务连接失败")
        ui.onAllNodes(isDialog()).assertCountEquals(1)
    }

}
