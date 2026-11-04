package com.hanxi.ghosterm

import android.graphics.Bitmap
import android.graphics.Paint
import android.graphics.Typeface
import android.net.Uri
import android.os.SystemClock
import android.provider.DocumentsContract
import android.view.accessibility.AccessibilityNodeInfo
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.FontDocumentReader
import com.hanxi.ghosterm.data.PreferenceUpdate
import com.hanxi.ghosterm.domain.*
import com.hanxi.ghosterm.terminal.*
import kotlinx.coroutines.*
import org.junit.*
import org.junit.Assert.*
import java.io.File
import java.util.UUID
import kotlin.math.ceil

class TerminalAppearanceProductTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private val instrumentation get()=InstrumentationRegistry.getInstrumentation()
    private val automation get()=instrumentation.uiAutomation
    private fun model()=ViewModelProvider(ui.activity)[SettingsViewModel::class.java]
    private fun sessions()=ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
    private fun fonts()=ViewModelProvider(ui.activity)[TerminalFontsViewModel::class.java]
    private val owner get()=(ui.activity.application as GhostermApplication).settings
    private lateinit var preferences:File
    private var before:ByteArray?=null
    private var oldFontIds=emptySet<String>()
    private var originalFlags=0
    private var originalRotation=0
    private var autoRotation=0
    private lateinit var store:TerminalFontStore
    private fun control(method:String,id:String?=null)=instrumentation.targetContext.contentResolver.call(
        Uri.parse("content://com.hanxi.ghosterm.test.fontcontrol"),method,id,null)!!
    private fun uri(id:String)=DocumentsContract.buildDocumentUri("com.hanxi.ghosterm.test.fonts",id)
    private fun await(label:String,block:()->Boolean) {
        val end=SystemClock.uptimeMillis()+10000
        while(SystemClock.uptimeMillis()<end) { if(ui.runOnIdle(block))return;SystemClock.sleep(25) }
        fail(label+" settings=${model().error}, fonts=${fonts().error}, session=${sessions().error}")
    }
    @Before fun setup() {
        originalFlags=automation.serviceInfo.flags
        automation.serviceInfo=automation.serviceInfo.apply { flags=flags or android.accessibilityservice.AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS }
        originalRotation=ui.activity.windowManager.defaultDisplay.rotation
        autoRotation=android.provider.Settings.System.getInt(ui.activity.contentResolver,android.provider.Settings.System.ACCELEROMETER_ROTATION,0)
        automation.setRotation(android.app.UiAutomation.ROTATION_FREEZE_0)
        await("Preferences not ready") { model().canEdit }
        preferences=File(ui.activity.filesDir,"preferences/preferences.json");before=preferences.takeIf { it.exists() }?.readBytes()
        store=TerminalFontStore(ui.activity);oldFontIds=store.list().map { it.id }.toSet()
        control("reset")
        assertTrue(runBlocking { owner.save(UUID.randomUUID(),PreferenceEdit.defaults()) } is PreferenceUpdate.Saved)
    }
    @After fun cleanup() {
        control("release");control("revoke")
        ui.runOnIdle { fonts().cancelImport();sessions().sessions.toList().forEach { sessions().close(it.id) } }
        await("Preference writes still pending") { owner.pendingCount.value==0 }
        store.list().filter { it.id !in oldFontIds }.forEach { store.remove(it.id) }
        before?.let(preferences::writeBytes) ?: preferences.delete()
        runBlocking { owner.reload(UUID.randomUUID()) }
        automation.serviceInfo=automation.serviceInfo.apply { flags=originalFlags }
        automation.setRotation(if(autoRotation==1) android.app.UiAutomation.ROTATION_UNFREEZE else originalRotation)
    }
    private fun appearance() {
        ui.onNodeWithContentDescription("设置").performClick()
        ui.onNodeWithTag("settings-section-APPEARANCE").performScrollTo().performClick()
    }
    private fun back()=ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
    private fun capture(name:String) {
        ui.waitForIdle();ui.mainClock.advanceTimeBy(2000);SystemClock.sleep(350)
        val image=checkNotNull(automation.takeScreenshot())
        val dir=File(ui.activity.cacheDir,"appearance-review").apply { mkdirs() }
        File(dir,"$name.png").outputStream().use { image.compress(Bitmap.CompressFormat.PNG,100,it) }
    }
    @Test fun searchableThemeLibraryFiltersAndPersistsTheActualChoiceAcrossRecreation() {
        appearance();ui.onNodeWithTag("settings-row-terminalTheme").performClick()
        ui.onNodeWithTag("theme-search").performTextInput("no-such-theme")
        ui.onNodeWithTag("theme-empty").assertIsDisplayed()
        ui.onNodeWithTag("theme-search").performTextClearance()
        ui.onNodeWithTag("theme-filter-light").performClick()
        ui.onAllNodes(isSelected() and hasTestTag("settings-option-terminalTheme-graphite")).assertCountEquals(0)
        ui.onNodeWithTag("settings-option-terminalTheme-graphite").assertDoesNotExist()
        ui.onNodeWithTag("theme-filter-all").performClick()
        ui.onNodeWithTag("theme-search").performTextInput("Dracula")
        capture("theme-library")
        ui.onNodeWithTag("settings-option-terminalTheme-ghostty:Dracula").performClick()
        await("Dracula not committed") { owner.state.value.committed.terminalTheme.id=="ghostty:Dracula" && model().inFlight==0 }
        assertEquals(0x282a36,sessions().terminalStyle.appearance.background)
        ui.activityRule.scenario.recreate()
        await("Selected theme did not survive recreation") { model().candidate.terminalTheme.label=="Dracula" && model().canEdit }
        ui.onNodeWithTag("settings-row-terminalTheme").assertTextContains("Dracula")
        capture("dracula-preview")
    }
    @Test fun selectedFontsChangeTheExistingViewMetricsAndRenderingWithoutReplacingThePty() {
        ui.onNodeWithText("本地终端").performClick()
        await("Local terminal did not connect") { sessions().active?.state?.connected==true }
        val session=ui.runOnIdle { sessions().active!! }
        val connection=session.connection;val surface=session.surface
        val pid=connection.state().child;assertTrue(pid>0)
        ui.terminalBack();appearance()
        ui.onNodeWithTag("settings-row-fontFamily").performScrollTo().performClick()
        await("Font catalog did not load") { !fonts().busy && fonts().choices.size>=3 }
        capture("font-library")
        ui.onNodeWithTag("settings-option-fontFamily-jetbrains_mono").performClick()
        await("Font choice did not reach live style") { owner.state.value.committed.fontFamily==TerminalFont.JETBRAINS_MONO && sessions().terminalStyle.fontId=="jetbrains_mono" }
        capture("font-preview")
        back();back();ui.onNodeWithText("继续本地终端").performClick()
        ui.waitForIdle()
        val view=findTerminal(ui.activity.window.decorView)!!
        val metrics=TerminalView::class.java.getDeclaredField("metricsPaint").apply { isAccessible=true }.get(view) as Paint
        val glyphs=TerminalView::class.java.getDeclaredField("paint").apply { isAccessible=true }.get(view) as Paint
        assertSame(glyphs.typeface,metrics.typeface)
        assertSame(TerminalFonts.typeface(ui.activity,"jetbrains_mono"),glyphs.typeface)
        val expectedFace=Typeface.CustomFallbackBuilder(android.graphics.fonts.FontFamily.Builder(
            android.graphics.fonts.Font.Builder(ui.activity.resources,com.hanxi.ghosterm.terminal.R.font.jetbrains_mono).build()).build())
            .setSystemFallback("monospace").build()
        fun pixels(face:Typeface):IntArray {
            val bitmap=Bitmap.createBitmap(180,60,Bitmap.Config.ARGB_8888)
            val paint=Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface=face;textSize=32f;color=android.graphics.Color.BLACK }
            android.graphics.Canvas(bitmap).drawText("Aa0Mwi",0f,40f,paint)
            return IntArray(180*60).also { bitmap.getPixels(it,0,180,0,0,180,60);bitmap.recycle() }
        }
        assertArrayEquals("Selected face must render the actual bundled JetBrains glyphs",pixels(expectedFace),pixels(glyphs.typeface))
        val expected=ceil(metrics.measureText("M")).toInt()
        await("Native grid does not follow the selected font") { connection.frame(-1)?.cols==view.width/expected }
        assertSame(connection,sessions().active!!.connection);assertSame(surface,sessions().active!!.surface)
        assertEquals(pid,connection.state().child)
        assertTrue(sessions().active!!.state.connected)
        capture("font-live-terminal")
    }
    private fun findTerminal(view:android.view.View):TerminalView? {
        if(view is TerminalView)return view
        if(view is android.view.ViewGroup)for(i in 0 until view.childCount)findTerminal(view.getChildAt(i))?.let { return it }
        return null
    }
    @Test fun realDocumentImportPublishesOneCopyRequiresSelectionAndRejectsProducerFailure() {
        appearance();ui.onNodeWithTag("settings-row-fontFamily").performScrollTo().performClick()
        await("Font list not loaded") { !fonts().busy }
        control("grant","regular")
        ui.runOnIdle { assertTrue(fonts().beginImport());fonts().completeImport(uri("regular")) }
        await("Font import did not finish") { !fonts().busy }
        assertNull(fonts().error)
        val imported=store.list().single { it.id !in oldFontIds }
        assertEquals(TerminalFont.SYSTEM,owner.state.value.committed.fontFamily)
        capture("imported-font")
        ui.onNodeWithTag("settings-option-fontFamily-${imported.id}").performClick()
        await("Imported font not committed") { owner.state.value.committed.fontFamily.id==imported.id && model().inFlight==0 }
        ui.activityRule.scenario.recreate()
        ui.onNodeWithTag("settings-row-fontFamily").performScrollTo().performClick()
        await("Imported font not reloaded") { !fonts().busy }
        ui.onNodeWithTag("settings-option-fontFamily-${imported.id}").assertIsSelected()
        ui.onNodeWithTag("font-remove-${imported.id}").assertIsNotEnabled()
        ui.onNodeWithTag("settings-option-fontFamily-system").performClick()
        await("System font not committed") { owner.state.value.committed.fontFamily==TerminalFont.SYSTEM && model().inFlight==0 }
        ui.onNodeWithTag("settings-row-fontFamily").performScrollTo().performClick();await("Font reload not complete") { !fonts().busy }
        ui.onNodeWithTag("font-remove-${imported.id}").performClick();ui.onNodeWithText("保留").performClick()
        assertTrue(store.verifiedFile(imported.id).exists())
        ui.onNodeWithTag("font-remove-${imported.id}").performClick();ui.onNodeWithTag("font-remove-confirm").performClick()
        await("Font removal not complete") { !fonts().busy && store.list().none { it.id==imported.id } }
        control("grant","error")
        ui.runOnIdle { assertTrue(fonts().beginImport());fonts().completeImport(uri("error")) }
        await("Producer error did not finish") { !fonts().busy }
        assertNotNull(fonts().error);assertTrue(store.list().none { it.id !in oldFontIds })
        for(document in listOf("oversize","invalid")) {
            control("grant",document)
            ui.runOnIdle { assertTrue(fonts().beginImport());fonts().completeImport(uri(document)) }
            await("Invalid font read did not finish") { !fonts().busy }
            assertNotNull("Invalid document must report an error",fonts().error)
            assertTrue("Rejected document cannot publish a font",store.list().none { it.id !in oldFontIds })
        }
        assertEquals(0,control("status").getInt("writes"))
    }
    @Test fun cancellingAFontReadClosesItsPipeAndLateResultCannotImport() {
        appearance();ui.onNodeWithTag("settings-row-fontFamily").performScrollTo().performClick();await("Font list not loaded") { !fonts().busy }
        control("grant","slow")
        ui.runOnIdle { assertTrue(fonts().beginImport());fonts().completeImport(uri("slow")) }
        await("Font reader not opened") { control("status").getInt("reads")==1 }
        ui.runOnIdle { fonts().cancelImport() }
        await("Cancelled reader retained its descriptor") { !fonts().busy && control("status").getInt("closed")==1 }
        control("release")
        ui.runOnIdle { fonts().completeImport(uri("slow")) }
        assertTrue(store.list().none { it.id !in oldFontIds });assertEquals(1,control("status").getInt("reads"))
    }
    private fun nodes(root:AccessibilityNodeInfo?=automation.rootInActiveWindow):List<AccessibilityNodeInfo> {
        val result=mutableListOf<AccessibilityNodeInfo>()
        fun visit(node:AccessibilityNodeInfo?) { if(node!=null) { result+=node;for(i in 0 until node.childCount)visit(node.getChild(i)) } }
        visit(root);return result
    }
    private fun systemAwait(block:()->Boolean) {
        val end=SystemClock.uptimeMillis()+10000
        while(SystemClock.uptimeMillis()<end) { if(block())return;SystemClock.sleep(50) }
        fail("System picker did not reach expected state: "+nodes().map { it.text to it.contentDescription })
    }
    private fun click(vararg labels:String):Boolean {
        for(match in nodes().filter { it.isVisibleToUser && (it.text?.toString() in labels || it.contentDescription?.toString() in labels) }) {
            when(tapLegacyDocumentRow(instrumentation,match)) { true -> return true;false -> continue;null -> Unit }
            var node:AccessibilityNodeInfo?=match
            while(node!=null) { if(node.isClickable && node.performAction(AccessibilityNodeInfo.ACTION_CLICK))return true;node=node.parent }
        }
        return false
    }
    @Test fun actualSystemPickerKeepsItsImportAcrossRotationAndReadsTheSelectedDocumentOnce() {
        appearance();ui.onNodeWithTag("settings-row-fontFamily").performScrollTo().performClick();await("Fonts not ready") { !fonts().busy }
        val previous=fonts();val oldActivity=ui.activity
        ui.onNodeWithTag("font-import").performClick()
        systemAwait { nodes().any { it.packageName?.toString()?.contains("documentsui")==true } }
        automation.setRotation(android.app.UiAutomation.ROTATION_FREEZE_90)
        systemAwait {
            val bounds=android.graphics.Rect();automation.rootInActiveWindow?.getBoundsInScreen(bounds)
            bounds.width()>bounds.height()
        }
        systemAwait {
            val tree=nodes();val roots=tree.firstOrNull { it.isVisibleToUser && it.viewIdResourceName?.endsWith(":id/roots_list")==true }
            if(roots!=null) {
                val row=nodes(roots).firstOrNull { it.isVisibleToUser && it.text?.toString()=="Ghosterm Test Fonts" }
                if(row!=null) {
                    var target:AccessibilityNodeInfo?=row
                    while(target!=null && !target.isClickable)target=target.parent
                    if(target?.performAction(AccessibilityNodeInfo.ACTION_CLICK)!=true)tapLegacyDocumentRow(instrumentation,row)
                } else roots.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)
                false
            } else if(click("regular.ttf")) true else {
                if(tree.any { it.isVisibleToUser && it.text?.toString()=="Ghosterm Test Fonts" })tree.firstOrNull { it.isScrollable && it.viewIdResourceName?.endsWith(":id/dir_list")==true }?.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)
                else click("Show roots","Open navigation drawer","显示根目录","显示位置")
                false
            }
        }
        systemAwait {
            val tree=nodes()
            if(tree.any { it.isVisibleToUser && it.packageName?.toString()?.contains("documentsui")==true })click("SELECT","Select","选择")
            tree.any { it.isVisibleToUser && it.packageName?.toString()=="com.hanxi.ghosterm" } && tree.none { it.packageName?.toString()?.contains("documentsui")==true }
        }
        await("System picker result was not imported") { !fonts().busy && !fonts().pickerPending && store.list().any { it.id !in oldFontIds } }
        assertTrue(oldActivity.isDestroyed);assertSame(previous,fonts());assertNull(fonts().error)
        assertEquals(1,control("status").getInt("reads"));assertEquals(0,control("status").getInt("writes"))
        capture("font-system-picker-result")
    }
}
