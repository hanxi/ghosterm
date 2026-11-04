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
}
