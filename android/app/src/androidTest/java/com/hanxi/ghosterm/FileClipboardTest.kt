package com.hanxi.ghosterm

import android.app.Application
import android.content.ClipData
import android.content.ClipboardManager
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.LocalFileBrowser
import com.hanxi.ghosterm.domain.*
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class FileClipboardTest {
    @get:Rule val ui=createAndroidComposeRule<ComponentActivity>()
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var model:FilesViewModel
    private lateinit var local:LocalFileBrowser
    private lateinit var app:Application
    private var previews=0
    private var lists=0
    @Before fun setup() {
        val context=InstrumentationRegistry.getInstrumentation().targetContext
        root=File(context.cacheDir,"clipboard-${UUID.randomUUID()}").apply { mkdir() }
        home=File(root,"home").apply { mkdir() }
        File(home,"folder").mkdir();File(home,"folder/child.txt").writeText("child")
        File(home,"a.txt").writeText("alpha");File(home,"b.txt").writeText("beta")
        File(home,".hidden").writeText("hidden")
        app=object:Application() { init { attachBaseContext(context) };override fun getFilesDir()=root }
        local=LocalFileBrowser(home,context.contentResolver)
        model=FilesViewModel(app,object:FileBrowserSource by local {
            override suspend fun browse(location:FileLocation):BrowserDirectory { lists++;return local.browse(location) }
            override suspend fun preview(location:FileLocation,entry:BrowserEntry):String { previews++;return local.preview(location,entry) }
        })
    }
    @After fun cleanup() { ui.runOnIdle { model.leave() };root.deleteRecursively() }
    private fun show() { ui.setContent { MaterialTheme { if(model.visible) FilesScreen(model,{}) } };ui.runOnIdle { model.enter() };loaded() }
    private fun loaded() { ui.waitUntil(10000) { !model.loading } }
    private fun entry(name:String)=ui.onNodeWithTag("file-entry:$name")
    private fun copy(name:String) { entry(name).performTouchInput { longClick() };ui.onNodeWithTag("file-copy").performClick() }
    private fun capture(name:String) {
        val instrumentation=InstrumentationRegistry.getInstrumentation()
        val bitmap=ui.onNodeWithTag(if(name=="card") "file-clipboard" else "file-clipboard-items").captureToImage().asAndroidBitmap()
        File(instrumentation.targetContext.cacheDir,"clipboard-$name.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG,100,it) }
    }
    private fun preserved() {
        assertEquals("alpha",File(home,"a.txt").readText());assertEquals("beta",File(home,"b.txt").readText())
        assertEquals("child",File(home,"folder/child.txt").readText());assertEquals("hidden",File(home,".hidden").readText())
    }
    @Test fun snapshotOwnsImmutableMetadataAndRejectsAmbiguousSelections() {
        val endpoint=FileEndpoint("remote-session","alias","/",remote=RemoteFileIdentity("alias","::1","user",2222))
        val item=BrowserEntry("/work/raw","名称",FileKind.FILE,ULong.MAX_VALUE,1234)
        val input=mutableListOf(item)
        val first=FileClipboardSnapshot.capture(FileLocation(endpoint,"/work"),"/work",input)
        input[0]=item.copy(id="changed",name="changed",size=null)
        assertEquals(item,first.items.single());assertEquals(endpoint,first.source.endpoint)
        assertEquals("/work",first.source.directoryId);assertEquals("/work",first.sourcePathLabel)
        assertTrue(first.createdAtMillis>0);UUID.fromString(first.id)
        val second=FileClipboardSnapshot.capture(first.source,"/work",listOf(item))
        assertNotEquals(first.id,second.id)
        var rejected=false
        try { (first.items as MutableList).clear() } catch(_:UnsupportedOperationException) { rejected=true }
        assertTrue("published list must not be mutable",rejected);assertEquals(listOf(item),first.items)
        for(bad in listOf(emptyList(),listOf(item,item),List(4097) { item.copy(id="$it") })) {
            var failed=false
            try { FileClipboardSnapshot.capture(first.source,"/work",bad) } catch(_:IllegalArgumentException) { failed=true }
            assertTrue("invalid selection cannot become a clipboard",failed)
        }
    }
    @Test fun copyExitsSelectionWithoutReadingOrChangingSystemClipboard() {
        show()
        val system=ui.activity.getSystemService(ClipboardManager::class.java)
        val before=system.primaryClip
        try {
            ui.runOnIdle { system.setPrimaryClip(ClipData.newPlainText("fixture","text clipboard stays")) }
            val count=lists;copy("a.txt")
            assertFalse(model.selecting);assertTrue(model.selected.isEmpty());assertNull(model.preview)
            assertEquals(listOf("a.txt"),model.clipboard!!.items.map { it.id })
            assertEquals(FileLocation(FileEndpoint.APP),model.clipboard!!.source)
            assertEquals(count,lists);assertEquals(0,previews)
            ui.onNodeWithTag("file-clipboard").assertIsDisplayed()
            ui.onNodeWithTag("file-clipboard-source").assertTextEquals("本机 · 应用文件")
            ui.onNodeWithTag("file-clipboard-path").assertTextEquals("应用文件")
            ui.onNodeWithTag("file-clipboard-names").assertTextEquals("a.txt")
            capture("card")
            assertEquals("text clipboard stays",system.primaryClip!!.getItemAt(0).text.toString());preserved()
        } finally { ui.runOnIdle { if(before!=null) system.setPrimaryClip(before) else system.clearPrimaryClip() } }
    }
    @Test fun zeroSelectionIsDisabledAndAllCopiesOnlyVisibleTopLevelEntries() {
        show();ui.onNodeWithText("选择").performClick();ui.onNodeWithTag("file-copy").assertIsNotEnabled()
        ui.runOnIdle { model.copySelected() };assertNull(model.clipboard)
        ui.onNodeWithText("全选").performClick();ui.onNodeWithTag("file-copy").performClick()
        val items=model.clipboard!!.items
        assertEquals(setOf("folder","a.txt","b.txt"),items.map { it.id }.toSet())
        assertEquals(FileKind.DIRECTORY,items.single { it.name=="folder" }.kind)
        assertFalse(model.selecting);preserved()
    }
    @Test fun replacementAndClearRequireExplicitDecisionsAndKeepOriginalFiles() {
        show();copy("a.txt");val first=model.clipboard!!
        copy("b.txt");assertSame(first,model.clipboard)
        ui.onNodeWithText("保留原清单").performClick();assertSame(first,model.clipboard);assertTrue(model.selecting)
        ui.onNodeWithTag("file-clipboard").assertIsDisplayed()
        ui.onNodeWithTag("file-copy").performClick()
        val pending=model.clipboardReplacement!!.snapshot
        ui.runOnIdle { model.clearChecked() }
        ui.onNodeWithText("替换清单").performClick()
        assertSame(pending,model.clipboard);assertNotEquals(first.id,model.clipboard!!.id)
        assertEquals("b.txt",model.clipboard!!.items.single().id);assertFalse(model.selecting)
        ui.onNodeWithText("清空").performClick();ui.onNodeWithText("取消").performClick()
        assertSame(pending,model.clipboard)
        ui.onNodeWithText("清空").performClick();ui.onNodeWithText("确认清空").performClick()
        assertNull(model.clipboard);ui.onNodeWithTag("file-clipboard").assertDoesNotExist();preserved()
    }
    @Test fun navigationLeaveAndReloadKeepFrozenOriginAndCancelOldReplacement() {
        show();copy("a.txt");val snapshot=model.clipboard!!
        entry("folder").performClick();loaded()
        ui.onNodeWithTag("file-clipboard-path").assertTextEquals("应用文件")
        copy("child.txt");assertNotNull(model.clipboardReplacement)
        ui.runOnIdle { model.leave();model.enter() };loaded()
        ui.runOnIdle { model.confirmClipboardReplacement();model.confirmClipboardClear() }
        assertSame(snapshot,model.clipboard);assertNull(model.clipboardReplacement)
        assertEquals("folder",model.location.directoryId)
        File(home,"a.txt").writeText("changed after copy")
        ui.runOnIdle { model.reload() };loaded()
        assertEquals(5uL,snapshot.items.single().size)
        assertEquals("a.txt",snapshot.items.single().name);assertEquals("",snapshot.source.directoryId)
        assertEquals(0,previews)
    }
    @Test fun detailsShowEveryNameAndBackClosesOnlyTheSheet() {
        repeat(14) { File(home,"名称-$it.txt").writeText("$it") }
        show();ui.onNodeWithText("选择").performClick();ui.onNodeWithText("全选").performClick()
        ui.onNodeWithTag("file-copy").performClick();val snapshot=model.clipboard!!
        assertEquals(17,snapshot.items.size)
        ui.onNodeWithText("查看清单").performClick()
        ui.onNodeWithTag("file-clipboard-detail-path").assertTextEquals("应用文件")
        for(item in snapshot.items) {
            ui.onNodeWithTag("file-clipboard-items").performScrollToNode(hasTestTag("file-clipboard-item:${item.id}"))
            ui.onNodeWithTag("file-clipboard-item:${item.id}").assertIsDisplayed()
        }
        capture("many-items")
        ui.onNodeWithText("返回文件").performClick()
        assertFalse(model.clipboardDetails);assertTrue(model.visible);assertSame(snapshot,model.clipboard)
        ui.onNodeWithTag("file-clipboard").assertIsDisplayed();preserved()
    }
}
