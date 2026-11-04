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
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
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
        if(!imeShown()) ui.showTerminalKeyboard()
        await("Actual system IME did not show") { imeShown() }
        ui.waitForIdle()
        await("Window must report the actual IME-constrained visible frame") {
            ui.runOnIdle { Rect().also { ui.activity.window.decorView.getWindowVisibleDisplayFrame(it) }.height() < 200 }
        }
        await("Compose compact controls must reflect the actual IME insets") {
            ui.onAllNodesWithTag("terminal-input-bar").fetchSemanticsNodes()
                .singleOrNull()?.boundsInRoot?.height?.let { it >= 47f } == true
        }
    }

    private fun visibleControlsAndPixels() {
        ui.onAllNodesWithTag("terminal-keyboard").assertCountEquals(0)
        ui.onAllNodesWithTag("terminal-menu").assertCountEquals(1)
        val bar = ui.onNodeWithTag("terminal-input-bar").assertIsDisplayed().fetchSemanticsNode().boundsInRoot
        val density = ui.activity.resources.displayMetrics.density
        assertEquals("landscape controls must still occupy exactly one 48dp row",48f*density,bar.height,density)
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
        for(tag in listOf("terminal-menu")) {
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

    @Test fun statusBarToggleAndLeavingTerminalRestoreSystemChromeWithoutReplacingPty() {
        fun statusShown(): Boolean {
            // Before API 30 compat visibility is inferred from stable insets,
            // which retain 24dp even when the actual system bar is hidden.
            if (android.os.Build.VERSION.SDK_INT < 30) return Regex(
                "BarController.StatusBar\\s+mState=WINDOW_STATE_SHOWING").containsMatchIn(shell("dumpsys window displays"))
            return ui.runOnIdle { ViewCompat.getRootWindowInsets(ui.activity.window.decorView)
                ?.isVisible(WindowInsetsCompat.Type.statusBars()) == true }
        }
        await("terminal must hide the actual system status bar") { !statusShown() }
        fun fillsTop() = ui.runOnIdle {
            val caption = ViewCompat.getRootWindowInsets(ui.activity.window.decorView)!!
                .getInsets(WindowInsetsCompat.Type.captionBar()).top
            IntArray(2).also { view().getLocationOnScreen(it) }[1] == caption
        }
        await("hidden status bar must release the entire top band, including cutout padding") { fillsTop() }
        ui.onNodeWithTag("terminal-menu").performClick()
        ui.onNodeWithTag("terminal-menu-status-bar").performScrollTo().assertTextEquals("显示系统状态栏").performClick()
        await("menu must restore the actual status bar") { statusShown() }
        await("shown status bar and camera cutout must not obscure the terminal") { ui.runOnIdle {
            val safeTop=ViewCompat.getRootWindowInsets(ui.activity.window.decorView)!!.getInsets(
                WindowInsetsCompat.Type.statusBars() or WindowInsetsCompat.Type.displayCutout()).top
            IntArray(2).also { view().getLocationOnScreen(it) }[1] == safeTop
        } }
        sameShell()
        ui.activityRule.scenario.recreate()
        await("visible status bar preference survives recreation") { statusShown() && find(ui.activity.window.decorView)?.currentFrame!=null }
        ui.onNodeWithTag("terminal-menu").performClick()
        ui.onNodeWithTag("terminal-menu-status-bar").performScrollTo().assertTextEquals("隐藏系统状态栏").performClick()
        await("menu must hide the actual status bar") { !statusShown() }
        await("hiding status bar again must release the top band") { fillsTop() }
        ui.openTerminalFiles()
        ui.onNodeWithTag("file-location").assertIsDisplayed()
        await("file page must regain the actual system status bar") { statusShown() }
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        await("returning terminal hides status bar") { !statusShown() }
        sameShell()
        ui.onNodeWithTag("terminal-menu").performClick()
        ui.onNodeWithTag("terminal-menu-home").performScrollTo().performClick()
        await("home must regain the actual system status bar") { statusShown() }
        assertSame(session,model().active)
    }

    @Test fun menuKeepsOpenKeyboardAndTerminalGeometryStable() {
        ui.showTerminalKeyboard()
        await("actual IME and terminal insets must settle") { imeShown() && terminalImeShown() }
        ui.waitForIdle()
        val height = ui.runOnIdle { view().height }
        val violations = mutableListOf<String>()
        val observer = android.view.ViewTreeObserver.OnPreDrawListener {
            val terminal = view()
            val shown = ViewCompat.getRootWindowInsets(terminal)
                ?.isVisible(WindowInsetsCompat.Type.ime()) == true
            if (!shown || terminal.height != height || !terminal.hasWindowFocus())
                violations += "ime=$shown height=${terminal.height}/$height focus=${terminal.hasWindowFocus()}"
            true
        }
        ui.runOnIdle { ui.activity.window.decorView.viewTreeObserver.addOnPreDrawListener(observer) }
        try {
            ui.onNodeWithTag("terminal-menu").performClick()
            ui.onNodeWithTag("terminal-menu-keyboard").performScrollTo().assertIsDisplayed()
            SystemClock.sleep(600)
            assertTrue("menu must keep the real system IME open", imeShown())
            ui.onNodeWithTag("terminal-menu-keyboard").performClick()
            SystemClock.sleep(600)
            ui.runOnIdle { assertTrue("menu open/close must retain IME, focus and viewport: $violations", violations.isEmpty()) }
            sameShell()
        } finally {
            ui.runOnIdle { ui.activity.window.decorView.viewTreeObserver.removeOnPreDrawListener(observer) }
        }
        ui.terminalBack()
        await("Back must still hide the actual IME") { !imeShown() }
        ui.onNodeWithTag("terminal-menu").performClick()
        ui.onNodeWithTag("terminal-menu-keyboard").performScrollTo().assertIsDisplayed()
        ui.terminalBack()
        ui.onNodeWithTag("terminal-menu-keyboard").assertDoesNotExist()
        sameShell()
    }

    @Test fun portraitFocusKeepsMenuFixedAndKeyboardInMenu() {
        val settings=(ui.activity.application as GhostermApplication).settings
        previousKeys=settings.state.value.committed.keySet
        previousAux=settings.state.value.committed.auxKeys
        assertTrue(runBlocking { settings.save(UUID.randomUUID(),listOf(
            PreferenceEdit.KeySet(AuxiliaryKeySet.EXTENDED),PreferenceEdit.AuxKeys(true))) } is PreferenceUpdate.Saved)
        ui.onAllNodesWithTag("terminal-keyboard").assertCountEquals(0)
        ui.onAllNodesWithTag("terminal-menu").assertCountEquals(1)
        val bar=ui.onNodeWithTag("terminal-input-bar").assertIsDisplayed().fetchSemanticsNode().boundsInRoot
        val scale=ui.activity.resources.displayMetrics.density
        assertEquals("input chrome occupies one row",48f*scale,bar.height,scale)
        for(tag in listOf("terminal-sessions","terminal-files")) {
            ui.onNodeWithTag(tag).assertDoesNotExist()
        }
        val top=ui.runOnIdle { IntArray(2).also { view().getLocationOnScreen(it) }[1] }
        val cutout=ui.runOnIdle { ViewCompat.getRootWindowInsets(ui.activity.window.decorView)!!
            .getInsets(WindowInsetsCompat.Type.captionBar()).top }
        assertEquals("terminal uses the hidden status band below any window caption",cutout,top)
        val menu=ui.onNodeWithTag("terminal-menu").fetchSemanticsNode().touchBoundsInRoot
        ui.onNodeWithText("Ctrl").assertIsDisplayed()
        ui.onNodeWithText("PgDn").performScrollTo().assertIsDisplayed()
        assertEquals("menu stays pinned",menu,ui.onNodeWithTag("terminal-menu").assertIsDisplayed().fetchSemanticsNode().touchBoundsInRoot)
        ui.onNodeWithText("Ctrl").assertIsNotDisplayed()
        ui.showTerminalKeyboard()
        await("Menu action must show the actual IME") {
            imeShown() && terminalImeShown()
        }
        ui.terminalBack()
        await("Back must close actual IME") { !imeShown() }
        assertTrue(runBlocking { settings.save(UUID.randomUUID(),listOf(PreferenceEdit.AuxKeys(false))) } is PreferenceUpdate.Saved)
        ui.onNodeWithTag("terminal-keyboard").assertDoesNotExist()
        ui.onAllNodesWithTag("terminal-auxiliary-keys").assertCountEquals(0)
        ui.onNodeWithTag("terminal-input-bar").assertIsDisplayed()
        ui.onNodeWithTag("terminal-menu").assertIsDisplayed()
        ui.showTerminalKeyboard()
        ui.terminalBack()
        await("Hidden auxiliary keys still allow system keyboard dismissal") { !imeShown() }
        ui.openTerminalSessions()
        ui.onNodeWithText("新建本地会话").assertIsDisplayed()
        androidx.test.espresso.Espresso.pressBack()
        ui.openTerminalFiles()
        ui.onNodeWithTag("file-location").assertIsDisplayed()
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        sameShell()
    }

    @Test fun landscapeImeKeepsRealPixelsControlsAndSamePty() {
        landscapeWithIme(1f);visibleControlsAndPixels();sameShell()
        ui.terminalBack()
        await("Compact back must hide the IME before leaving the session") { !imeShown() }
        await("Navigation must be visible after keyboard removal settles") {
            try { ui.onNodeWithTag("terminal-menu").assertIsDisplayed(); true } catch (_: AssertionError) { false }
        }
        sameShell()
        landscapeWithIme(1f);visibleControlsAndPixels()
        ui.terminalBack()
        await("Back must actually hide the IME") { !imeShown() }
        await("Navigation must settle after the keyboard action") {
            try { ui.onNodeWithTag("terminal-menu").assertIsDisplayed(); true } catch (_: AssertionError) { false }
        }
        ui.showTerminalKeyboard()
        ui.terminalBack()
        await("Back must hide IME again") { !imeShown() }
        sameShell()
        ui.terminalBack()
        ui.onNodeWithText("继续本地终端").assertIsDisplayed()
        assertSame("system back must retain the live PTY",session,model().active)
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

    @Test fun shortWindowScrollsAllKeysAndKeepsMenuAboveTheWindowEdge() {
        val settings=(ui.activity.application as GhostermApplication).settings
        previousKeys=settings.state.value.committed.keySet
        previousAux=settings.state.value.committed.auxKeys
        assertTrue(runBlocking { settings.save(UUID.randomUUID(),listOf(PreferenceEdit.KeySet(AuxiliaryKeySet.EXTENDED))) } is PreferenceUpdate.Saved)
        if(imeShown()) { ui.terminalBack(); await("IME must hide") { !imeShown() } }
        shell("wm size 640x200")
        ui.runOnUiThread { ui.activity.requestedOrientation=ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE }
        await("Fixture must reach the short native window") { ui.activity.resources.configuration.screenHeightDp <= 220 }
        val before=ui.onNodeWithTag("terminal-menu").assertIsDisplayed().fetchSemanticsNode().touchBoundsInRoot
        ui.onNodeWithText("PgDn").performScrollTo().assertIsDisplayed()
        val after=ui.onNodeWithTag("terminal-menu").assertIsDisplayed().fetchSemanticsNode().touchBoundsInRoot
        assertEquals(before.left,after.left,.5f)
        ui.onNodeWithText("Ctrl").performScrollTo().performClick();ui.runOnIdle { assertEquals(1,view().ctrl) }
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
