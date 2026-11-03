package com.hanxi.ghosterm

import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.Rect
import android.os.SystemClock
import android.view.View
import android.view.ViewGroup
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.PreferenceUpdate
import com.hanxi.ghosterm.domain.AuxiliaryKeySet
import com.hanxi.ghosterm.domain.TerminalLineHeight
import com.hanxi.ghosterm.terminal.TerminalView
import kotlinx.coroutines.runBlocking
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.io.FileInputStream
import java.util.UUID

/** Real system IME/configuration, visible-screen geometry, pixels and a retained shell. */
@RunWith(AndroidJUnit4::class)
class TerminalLayoutTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation get() = InstrumentationRegistry.getInstrumentation()
    private val system = linkedMapOf<String,String>()
    private var size = "reset"
    private var density = "reset"
    private var previousKeys: AuxiliaryKeySet? = null
    private var previousAux: Boolean? = null
    private var previousFont: Int? = null
    private var previousLineHeight: TerminalLineHeight? = null
    private lateinit var proof: File
    private lateinit var session: TerminalSession
    private var shellPid = ""
    private fun model() = ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
    private fun shell(command: String) = instrumentation.uiAutomation.executeShellCommand(command).use {
        FileInputStream(it.fileDescriptor).bufferedReader().use { reader -> reader.readText() }
    }
    private fun await(message: String, predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime()+15_000
        while (SystemClock.elapsedRealtime()<end) { ui.mainClock.advanceTimeByFrame();if(predicate()) return;SystemClock.sleep(40) }
        fail(message)
    }
    private fun find(view: View): TerminalView? = (view as? TerminalView) ?: (view as? ViewGroup)?.let { group ->
        (0 until group.childCount).firstNotNullOfOrNull { find(group.getChildAt(it)) }
    }
    private fun view() = checkNotNull(find(ui.activity.window.decorView))
    private fun imeShown() = "mInputShown=true" in shell("dumpsys input_method")
    private fun send(value: String) = ui.runOnIdle { assertTrue(view().commitText(value)) }

    @Before fun prepare() {
        for(key in listOf("font_scale","accelerometer_rotation","user_rotation")) system[key]=shell("settings get system $key").trim()
        size=shell("wm size").lineSequence().firstOrNull { it.startsWith("Override size:") }?.substringAfter(":")?.trim() ?: "reset"
        density=shell("wm density").lineSequence().firstOrNull { it.startsWith("Override density:") }?.substringAfter(":")?.trim() ?: "reset"
        shell("settings put system font_scale 1.0")
        shell("settings put system accelerometer_rotation 0")
        shell("settings put system user_rotation 0")
        shell("wm size 320x640");shell("wm density 160")
        ui.runOnUiThread { ui.activity.requestedOrientation=ActivityInfo.SCREEN_ORIENTATION_PORTRAIT }
        await("Fixture must reach a portrait phone window") { ui.activity.resources.configuration.orientation==Configuration.ORIENTATION_PORTRAIT }
        ui.onNodeWithText("本地终端").performClick()
        try {
            await("Fixture terminal did not connect") { ui.runOnIdle { model().active?.state?.connected==true && find(ui.activity.window.decorView)?.currentFrame!=null } }
        } catch (failure: AssertionError) {
            ui.runOnIdle { val current=model().active;val native=find(ui.activity.window.decorView)
                fail("Fixture terminal: state=${current?.state}, view=${native?.width}x${native?.height}, frame=${native?.currentFrame?.revision}, nativeFrame=${current?.connection?.frame(0)?.revision}, visible=${model().visible}, error=${model().error}") }
            throw failure
        }
        session=model().active!!
        proof=File(ui.activity.filesDir,"home/layout-${UUID.randomUUID()}.txt")
        send(NativePtyFixture.initialize()+"stty -echo; PS1=''; printf '%s\\n' \"\u0024\u0024\" > '${proof.absolutePath}'; printf '\\033[41m\\033[2J\\033[H'\r")
        await("Fixture did not record its real shell PID") { proof.isFile && proof.readText().trim().toIntOrNull()!=null }
        shellPid=proof.readText().trim()
    }

    @After fun restore() {
        try {
            ui.runOnIdle { model().sessions.map { it.id }.forEach(model()::close) }
            if(::proof.isInitialized) proof.delete()
            previousFont?.let { previous ->
                val settings=(ui.activity.application as GhostermApplication).settings
                assertTrue(runBlocking { settings.save(UUID.randomUUID(),listOf(PreferenceEdit.FontSize(previous),PreferenceEdit.LineHeight(checkNotNull(previousLineHeight)))) } is PreferenceUpdate.Saved)
            }
            previousKeys?.let { previous ->
                val settings=(ui.activity.application as GhostermApplication).settings
                assertTrue(runBlocking { settings.save(UUID.randomUUID(),listOf(PreferenceEdit.KeySet(previous),PreferenceEdit.AuxKeys(checkNotNull(previousAux)))) } is PreferenceUpdate.Saved)
            }
        } finally {
            ui.runOnUiThread { ui.activity.requestedOrientation=ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED }
            for((key,value) in system) shell(if(value=="null") "settings delete system $key" else "settings put system $key $value")
            shell("wm size $size");shell("wm density $density")
        }
    }

    private fun landscapeWithIme(font: Float) {
        shell("settings put system font_scale $font")
        ui.runOnUiThread { ui.activity.requestedOrientation=ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE }
        await("Landscape/font configuration did not settle") {
            ui.activity.resources.configuration.orientation==Configuration.ORIENTATION_LANDSCAPE &&
                kotlin.math.abs(ui.activity.resources.configuration.fontScale-font)<.01f
        }
        await("Rotated terminal view did not attach") { find(ui.activity.window.decorView)?.hasWindowFocus()==true }
        if(!imeShown()) ui.onNodeWithTag("terminal-keyboard").performClick()
        await("Actual system IME did not show") { imeShown() }
        ui.waitForIdle()
        await("Window must report the actual IME-constrained visible frame") {
            ui.runOnIdle { Rect().also { ui.activity.window.decorView.getWindowVisibleDisplayFrame(it) }.height() < 200 }
        }
        await("Compose compact controls must reflect the actual IME insets") {
            ui.onAllNodesWithTag("terminal-compact-bar").fetchSemanticsNodes()
                .singleOrNull()?.boundsInRoot?.height?.let { it >= 47f } == true
        }
    }

    private fun visibleControlsAndPixels() {
        ui.onAllNodesWithTag("terminal-keyboard").assertCountEquals(1)
        ui.onAllNodesWithTag("terminal-menu").assertCountEquals(1)
        lateinit var window: Rect
        lateinit var terminal: Rect
        ui.runOnIdle {
            window=Rect().also { ui.activity.window.decorView.getWindowVisibleDisplayFrame(it) }
            terminal=Rect();assertTrue("terminal must have visible screen area",view().getGlobalVisibleRect(terminal))
            val readable=kotlin.math.ceil(android.util.TypedValue.applyDimension(android.util.TypedValue.COMPLEX_UNIT_SP,model().terminalStyle.fontSize,view().resources.displayMetrics)*model().terminalStyle.lineHeight).toInt()
            assertTrue("terminal must retain at least a line of height: actual=${terminal.height()}, readable=$readable, window=$window, style=${model().terminalStyle}",terminal.height()>=readable)
            assertTrue("terminal bottom must remain above the IME",terminal.bottom<=window.bottom)
            assertSame(session.surface,view().surface)
            assertEquals(session.id,model().active!!.id)
        }
        for(tag in listOf("terminal-back","terminal-keyboard","terminal-menu")) {
            val bounds=ui.onNodeWithTag(tag).assertIsDisplayed().fetchSemanticsNode().touchBoundsInRoot
            assertTrue("$tag touch height",bounds.height>=47)
            assertTrue("$tag above IME",bounds.bottom<=window.bottom+1)
            assertTrue("$tag inside left edge",bounds.left>=window.left-1)
            assertTrue("$tag inside right edge",bounds.right<=window.right+1)
        }
        send("printf '\\033[41m\\033[2J\\033[H'\r")
        await("Live ANSI output must paint inside the actually visible terminal") {
            val image=instrumentation.uiAutomation.takeScreenshot() ?: return@await false
            try { val pixel=image.getPixel(terminal.centerX(),terminal.centerY())
                Color.red(pixel)>Color.green(pixel)+40 && Color.red(pixel)>Color.blue(pixel)+40
            } finally { image.recycle() }
        }
    }

    private fun sameShell() {
        assertTrue(proof.delete())
        send("printf '%s\\n' \"\u0024\u0024\" > '${proof.absolutePath}'\r")
        await("The same live PTY must accept input after the layout transition") { proof.isFile && proof.readText().trim()==shellPid }
        assertSame(session.surface,model().active!!.surface)
    }

    @Test fun portraitInputRowKeepsNavigationAtTopAndKeyboardPinned() {
        val settings=(ui.activity.application as GhostermApplication).settings
        previousKeys=settings.state.value.committed.keySet
        previousAux=settings.state.value.committed.auxKeys
        assertTrue(runBlocking { settings.save(UUID.randomUUID(),listOf(
            PreferenceEdit.KeySet(AuxiliaryKeySet.EXTENDED),PreferenceEdit.AuxKeys(true))) } is PreferenceUpdate.Saved)
        ui.onAllNodesWithTag("terminal-keyboard").assertCountEquals(1)
        ui.onAllNodesWithTag("terminal-menu").assertCountEquals(1)
        val bar=ui.onNodeWithTag("terminal-input-bar").assertIsDisplayed().fetchSemanticsNode().boundsInRoot
        val scale=ui.activity.resources.displayMetrics.density
        assertEquals("input chrome occupies one row",52f*scale,bar.height,scale)
        for(tag in listOf("terminal-sessions","terminal-files")) {
            val bounds=ui.onNodeWithTag(tag).assertIsDisplayed().fetchSemanticsNode().boundsInRoot
            assertTrue("navigation is above terminal input",bounds.bottom<bar.top)
        }
        val keyboard=ui.onNodeWithTag("terminal-keyboard").assertIsDisplayed().fetchSemanticsNode().touchBoundsInRoot
        val ctrl=ui.onNodeWithText("Ctrl").assertIsDisplayed().fetchSemanticsNode().touchBoundsInRoot
        ui.onNodeWithText("PgDn").performScrollTo().assertIsDisplayed()
        assertEquals("keyboard stays pinned",keyboard,ui.onNodeWithTag("terminal-keyboard").assertIsDisplayed().fetchSemanticsNode().touchBoundsInRoot)
        assertEquals("Ctrl stays pinned",ctrl,ui.onNodeWithText("Ctrl").assertIsDisplayed().fetchSemanticsNode().touchBoundsInRoot)
        ui.onNodeWithTag("terminal-keyboard").assertIsNotSelected().assertContentDescriptionEquals("显示键盘").performClick()
        await("Keyboard icon must reflect the actual IME") {
            imeShown() && ui.onAllNodesWithContentDescription("收起键盘").fetchSemanticsNodes().isNotEmpty()
        }
        ui.onNodeWithTag("terminal-keyboard").assertIsSelected().performClick()
        await("Keyboard toggle must close actual IME") { !imeShown() }
        assertTrue(runBlocking { settings.save(UUID.randomUUID(),listOf(PreferenceEdit.AuxKeys(false))) } is PreferenceUpdate.Saved)
        ui.onNodeWithTag("terminal-keyboard").assertIsDisplayed()
        ui.onAllNodesWithTag("terminal-auxiliary-keys").assertCountEquals(0)
        ui.onNodeWithTag("terminal-sessions").performClick()
        ui.onNodeWithText("新建本地会话").assertIsDisplayed()
        androidx.test.espresso.Espresso.pressBack()
        ui.onNodeWithTag("terminal-files").performClick()
        ui.onNodeWithTag("file-location").assertIsDisplayed()
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        sameShell()
    }

    @Test fun landscapeImeKeepsRealPixelsControlsAndSamePty() {
        landscapeWithIme(1f);visibleControlsAndPixels();sameShell()
        ui.onNodeWithTag("terminal-back").performClick()
        await("Compact back must hide the IME before leaving the session") { !imeShown() }
        await("Navigation must be visible after keyboard removal settles") {
            try { ui.onNodeWithTag("terminal-files").assertIsDisplayed(); true } catch (_: AssertionError) { false }
        }
        sameShell()
        landscapeWithIme(1f);visibleControlsAndPixels()
        ui.onNodeWithTag("terminal-keyboard").performClick()
        await("Keyboard action must actually hide the IME") { !imeShown() }
        await("Navigation must settle after the keyboard action") {
            try { ui.onNodeWithTag("terminal-files").assertIsDisplayed(); true } catch (_: AssertionError) { false }
        }
        ui.onNodeWithTag("terminal-keyboard").assert(hasText("显示键盘") or hasContentDescription("显示键盘"))
        sameShell()
    }

    @Test fun largeFontCompactMenuRoutesFilesAndSessionsWithoutReplacingOwner() {
        val settings=(ui.activity.application as GhostermApplication).settings
        previousFont=settings.state.value.committed.fontSize
        previousLineHeight=settings.state.value.committed.lineHeight
        assertTrue(runBlocking { settings.save(UUID.randomUUID(),listOf(PreferenceEdit.FontSize(14),PreferenceEdit.LineHeight(TerminalLineHeight.STANDARD))) } is PreferenceUpdate.Saved)
        landscapeWithIme(2f)
        await("Short landscape must preserve two lines, or all available height") {
            ui.runOnIdle {
                val line=kotlin.math.ceil(android.util.TypedValue.applyDimension(android.util.TypedValue.COMPLEX_UNIT_SP,model().terminalStyle.fontSize,view().resources.displayMetrics)*model().terminalStyle.lineHeight).toInt()
                val window=Rect().also { ui.activity.window.decorView.getWindowVisibleDisplayFrame(it) }
                view().height>=minOf(line*2,window.height())
            }
        }
        visibleControlsAndPixels()
        ui.onNodeWithTag("terminal-menu").performClick();ui.onNodeWithTag("terminal-menu-files").performScrollTo().performClick()
        ui.onNodeWithTag("file-location").assertExists()
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        landscapeWithIme(2f)
        ui.onNodeWithTag("terminal-menu").performClick();ui.onNodeWithTag("terminal-menu-sessions").performScrollTo().performClick()
        try { ui.onNodeWithText("新建本地会话").performScrollTo().assertIsDisplayed() }
        catch (failure: AssertionError) {
            instrumentation.uiAutomation.takeScreenshot()?.let { image ->
                File(ui.activity.cacheDir,"layout-sheet-debug.png").outputStream().use { image.compress(android.graphics.Bitmap.CompressFormat.PNG,100,it) };image.recycle()
            }
            throw failure
        }
        androidx.test.espresso.Espresso.pressBack()
        sameShell()
    }

    @Test fun shortWindowKeepsCtrlPinnedAndActionsAboveTheWindowEdge() {
        val settings=(ui.activity.application as GhostermApplication).settings
        previousKeys=settings.state.value.committed.keySet
        previousAux=settings.state.value.committed.auxKeys
        assertTrue(runBlocking { settings.save(UUID.randomUUID(),listOf(PreferenceEdit.KeySet(AuxiliaryKeySet.EXTENDED))) } is PreferenceUpdate.Saved)
        if(imeShown()) ui.onNodeWithTag("terminal-keyboard").performClick()
        shell("wm size 640x200")
        ui.runOnUiThread { ui.activity.requestedOrientation=ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE }
        await("Fixture must reach the short native window") { ui.activity.resources.configuration.screenHeightDp <= 220 }
        val before=ui.onNodeWithText("Ctrl").assertIsDisplayed().fetchSemanticsNode().touchBoundsInRoot
        ui.onNodeWithText("PgDn").performScrollTo().assertIsDisplayed()
        val after=ui.onNodeWithText("Ctrl").assertIsDisplayed().fetchSemanticsNode().touchBoundsInRoot
        assertEquals(before.left,after.left,.5f)
        ui.onNodeWithText("Ctrl").performClick();ui.runOnIdle { assertEquals(1,view().ctrl) }
        ui.onNodeWithText("Ctrl ✓").performClick();ui.runOnIdle { assertEquals(0,view().ctrl) }
        visibleControlsAndPixels()
        val height=ui.runOnIdle { view().height }
        ui.runOnIdle { model().reportError("布局错误可见") }
        ui.onNodeWithText("布局错误可见").assertIsDisplayed()
        ui.runOnIdle { assertEquals("error display must not consume the remaining terminal height",height,view().height) }
        ui.onNodeWithText("知道了").performClick()
        ui.runOnIdle { assertNull(model().error) }
        assertTrue(runBlocking { settings.save(UUID.randomUUID(),listOf(PreferenceEdit.AuxKeys(false))) } is PreferenceUpdate.Saved)
        ui.onAllNodesWithTag("terminal-auxiliary-keys").assertCountEquals(0)
        ui.onAllNodesWithText("Ctrl").assertCountEquals(0)
        assertTrue(runBlocking { settings.save(UUID.randomUUID(),listOf(PreferenceEdit.AuxKeys(true))) } is PreferenceUpdate.Saved)
        ui.onNodeWithText("Ctrl").assertIsDisplayed()
        sameShell()
    }
}
