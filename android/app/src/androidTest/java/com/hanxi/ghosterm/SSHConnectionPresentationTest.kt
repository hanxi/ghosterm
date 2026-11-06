package com.hanxi.ghosterm

import androidx.activity.ComponentActivity
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelStore
import com.hanxi.ghosterm.bridge.PreparedSSH
import kotlinx.coroutines.awaitCancellation
import org.junit.After
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test

class SSHConnectionPresentationTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private val store = ViewModelStore()
    @After fun cleanup() { ui.runOnIdle { store.clear() } }
    private fun setup(user: String = "user"): SessionsViewModel {
        val model = SessionsViewModel(ui.activity.application, SSHResolver { awaitCancellation() }, monitorSessions = false)
        ui.runOnUiThread {
            store.put("connection", model)
            model.connect(PreparedSSH("example", "example.invalid", user, 22, emptyList()))
        }
        // Exercise the real app routing, including the page's own BackHandler.
        ui.setContent { GhostermApp(sessions = model) }
        return model
    }
    @Test fun passiveStagesStayInOnePageIndicatorAndCancelCreatesNoSession() {
        val model = setup()
        val id = model.connecting!!.id
        for (message in listOf("正在解析主机地址…", "正在连接 SSH…", "正在认证…", "正在打开远端终端…")) {
            ui.runOnIdle { model.connecting!!.message = message }
            ui.onNodeWithTag("ssh-connection-progress").assertIsDisplayed()
            ui.onNodeWithText(message).assertIsDisplayed()
            ui.onAllNodes(isDialog()).assertCountEquals(0)
            ui.runOnIdle { assertEquals(id, model.connecting!!.id) }
        }
        ui.onNodeWithTag("ssh-connection-cancel").performClick()
        ui.onNodeWithTag("ssh-connection-progress").assertDoesNotExist()
        ui.runOnIdle { assertNull(model.connecting); assertTrue(model.sessions.isEmpty()) }
    }
    @Test fun backCancelsTheAttemptBeforePageNavigation() {
        val model = setup()
        ui.onNodeWithTag("ssh-connection-progress").assertIsDisplayed()
        ui.runOnIdle { ui.activity.onBackPressedDispatcher.onBackPressed() }
        ui.runOnIdle { assertNull(model.connecting); assertTrue(model.sessions.isEmpty()) }
        ui.onNodeWithTag("ssh-connection-progress").assertDoesNotExist()
        assertFalse(ui.activity.isFinishing)
    }
    @Test fun failureReplacesPassiveProgressWithAnExplicitError() {
        val model = setup()
        ui.runOnIdle { model.connecting!!.let { it.message = "SSH 连接超时"; it.failed = true } }
        ui.onNodeWithTag("ssh-connection-progress").assertDoesNotExist()
        ui.onAllNodes(isDialog()).assertCountEquals(1)
        ui.onNodeWithTag("ssh-failure").assertTextEquals("SSH 连接超时")
        ui.onNodeWithText("关闭").performClick()
        ui.runOnIdle { assertNull(model.connecting) }
    }
    @Test fun usernamePromptReturnsToInlineProgressAfterSubmission() {
        val model = setup("")
        ui.onNodeWithTag("ssh-connection-progress").assertDoesNotExist()
        ui.onAllNodes(isDialog()).assertCountEquals(1)
        ui.onNodeWithTag("ssh-username").performTextInput("user")
        ui.onNodeWithText("继续连接").performClick()
        ui.onAllNodes(isDialog()).assertCountEquals(0)
        ui.onNodeWithTag("ssh-connection-progress").assertIsDisplayed()
        ui.runOnIdle { assertEquals("user", model.connecting!!.username); assertEquals("", model.connecting!!.target.user) }
    }
    @Test fun tmuxManagementUsesInlineProgressAndBackCancels() {
        val model=SessionsViewModel(ui.activity.application, SSHResolver { awaitCancellation() }, monitorSessions=false)
        ui.runOnUiThread {
            store.put("connection",model)
            model.connectTmux(PreparedSSH("tmux-host","example.invalid","user",22,emptyList())) { remote,_ -> remote.close() }
        }
        ui.setContent { GhostermApp(sessions=model) }
        ui.onNodeWithTag("ssh-connection-progress").assertIsDisplayed()
        ui.onAllNodes(isDialog()).assertCountEquals(0)
        ui.runOnIdle { ui.activity.onBackPressedDispatcher.onBackPressed() }
        ui.runOnIdle { assertNull(model.connecting) }
        ui.onNodeWithTag("ssh-connection-progress").assertDoesNotExist()
    }

}
