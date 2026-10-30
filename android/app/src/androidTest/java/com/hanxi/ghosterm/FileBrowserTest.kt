package com.hanxi.ghosterm

import android.app.Application
import android.graphics.Bitmap
import androidx.compose.ui.graphics.asAndroidBitmap
import android.system.Os
import android.system.OsConstants
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.LocalFileBrowser
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine

@RunWith(AndroidJUnit4::class)
class FileBrowserTest {
    @get:Rule val ui=createAndroidComposeRule<ComponentActivity>()
    private lateinit var directory:File
    private lateinit var home:File
    private lateinit var app:Application
    private lateinit var source:LocalFileBrowser
    private lateinit var model:FilesViewModel
    private var sessions:SessionsViewModel?=null
    @Before fun setup() {
        val context=InstrumentationRegistry.getInstrumentation().targetContext
        directory=File(context.cacheDir,"files-${UUID.randomUUID()}").apply { mkdirs() }
        home=File(directory,"home").apply { mkdirs() }
        File(home,"Alpha").mkdir(); File(home,"Alpha/child.txt").writeText("child\n")
        File(home,"notes.txt").writeText("你好 🔑\nsecond line\n")
        File(home,".hidden").writeText("hidden")
        File(home,"空 文.txt").writeText("")
        File(directory,"identities").mkdir(); File(directory,"identities/container").writeText("private")
        app=object:Application() { init { attachBaseContext(context) }; override fun getFilesDir()=directory }
        source=LocalFileBrowser(home,context.contentResolver)
        model=FilesViewModel(app,source)
    }
    @After fun cleanup() {
        ui.runOnIdle { model.leave();sessions?.let { s -> s.sessions.map { it.id }.forEach(s::close) } }
        directory.deleteRecursively()
    }
    private fun show() {
        ui.setContent { MaterialTheme { if(model.visible) FilesScreen(model,{}) } }
        ui.runOnIdle { model.enter() }; loaded()
    }
    private fun loaded() { ui.waitUntil(10_000) { ui.runOnIdle { !model.loading } } }
    private fun entry(name:String)=ui.onNodeWithTag("file-entry:$name")
    private fun capture(name:String) {
        val bitmap=ui.onRoot().captureToImage().asAndroidBitmap()
        File(directory.parentFile,"files-browser-$name.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG,100,it) }
    }
    private fun root()=FileLocation(FileEndpoint.APP)
    private fun listed()=runBlocking { source.list(root()) }
    private fun rejects(block:suspend ()->Unit) {
        var failure:Exception?=null
        try { runBlocking { block() } } catch(e:Exception) { failure=e }
        assertNotNull("operation must explicitly fail",failure)
    }

    @Test fun realDirectoryKeepsMetadataAndNeverExposesInternalContainers() {
        val entries=listed()
        assertEquals("Alpha",entries.first().name)
        assertTrue(entries.any { it.name==".hidden" })
        assertFalse(entries.any { it.name=="identities" })
        assertEquals(null,entries.first().size)
        assertEquals(0L,entries.single { it.name=="空 文.txt" }.size)
        assertEquals("你好 🔑\nsecond line\n",runBlocking { source.preview(root(),entries.single { it.name=="notes.txt" }) })
        assertEquals("",runBlocking { source.preview(root(),entries.single { it.name=="空 文.txt" }) })
        assertEquals("private",File(directory,"identities/container").readText())
    }
    @Test fun tapOpensAndLongPressSelectsWithoutChangingDirectory() {
        show(); assertFalse(model.selecting);capture("browse")
        entry("Alpha").performClick(); loaded()
        entry("child.txt").assertIsDisplayed(); assertEquals("Alpha",model.location.directoryId)
        ui.onNodeWithText("返回").performClick();loaded()
        entry("notes.txt").performClick()
        ui.waitUntil { ui.runOnIdle { model.preview?.text!=null } }
        ui.onNodeWithTag("file-preview").assertTextEquals("你好 🔑\nsecond line\n")
        ui.onNodeWithText("返回").performClick()
        entry("Alpha").performTouchInput { longClick() }
        entry("Alpha").assertIsSelected(); assertEquals("",model.location.directoryId)
        entry("notes.txt").performClick();entry("notes.txt").assertIsSelected()
        assertNull(model.preview);assertEquals(2,model.selected.size);capture("selected")
        entry("Alpha").performClick();entry("Alpha").assertIsNotSelected()
        ui.onNodeWithText("返回").performClick()
        assertFalse(model.selecting);assertTrue(model.selected.isEmpty());assertTrue(model.visible)
        entry("notes.txt").performClick();ui.waitUntil { ui.runOnIdle { model.preview?.text!=null } }
        assertArrayEquals("你好 🔑\nsecond line\n".toByteArray(),File(home,"notes.txt").readBytes())
    }
    @Test fun selectAllUsesVisibleRowsAndHiddenTogglePrunesSelection() {
        show();ui.onNodeWithText("选择").performClick();ui.onNodeWithText("全选").performClick()
        assertEquals(setOf("Alpha","notes.txt","空 文.txt"),model.selected)
        assertFalse(model.selected.contains("Alpha/child.txt"))
        ui.onNodeWithText("更多").performClick();ui.onNodeWithText("显示隐藏文件").performClick()
        entry(".hidden").performClick();assertTrue(model.selected.contains(".hidden"))
        ui.onNodeWithText("更多").performClick();ui.onNodeWithText("隐藏点文件").performClick()
        assertFalse(model.selected.contains(".hidden"));entry(".hidden").assertDoesNotExist()
        ui.onNodeWithText("清除选择").performClick();assertTrue(model.selected.isEmpty());assertTrue(model.selecting)
        ui.onNodeWithText("取消选择").performClick();assertFalse(model.selecting)
    }
    @Test fun readFailureIsVisibleAndRetryDoesNotPretendEmpty() {
        val saved=File(directory,"moved-home");check(home.renameTo(saved));home.writeText("not a directory")
        show();ui.onNodeWithTag("file-list-error").assertIsDisplayed();ui.onNodeWithTag("file-list-empty").assertDoesNotExist()
        ui.onNodeWithText("选择").assertIsNotEnabled()
        check(home.delete());check(saved.renameTo(home))
        ui.onNodeWithText("重试").performClick();loaded();entry("notes.txt").assertIsDisplayed()
        home.listFiles()!!.forEach { it.deleteRecursively() }
        ui.runOnIdle { model.reload() };loaded()
        ui.onNodeWithTag("file-list-empty").assertIsDisplayed();ui.onNodeWithTag("file-list-error").assertDoesNotExist()
    }
    @Test fun previewRejectsOversizeMalformedAndNulWithoutWritingAnything() {
        for(bytes in listOf(ByteArray(LocalFileBrowser.PREVIEW_LIMIT+1){65},byteArrayOf(0xc3.toByte(),0x28),byteArrayOf(65,0,66))) {
            val file=File(home,"invalid.txt");file.writeBytes(bytes)
            rejects { source.preview(root(),listed().single { it.name==file.name }) }
            assertArrayEquals(bytes,file.readBytes())
        }
        val exact=ByteArray(LocalFileBrowser.PREVIEW_LIMIT){65};File(home,"exact.txt").writeBytes(exact)
        assertEquals(exact.size,runBlocking { source.preview(root(),listed().single { it.name=="exact.txt" }) }.length)
        assertEquals(setOf("home","identities"),directory.list()!!.toSet())
    }
    @Test fun linksTraversalAndSpecialFilesAreExplicitlyRejected() {
        val outside=File(directory,"outside").apply { mkdir() };File(outside,"secret").writeText("outside")
        Os.symlink(outside.path,File(home,"link").path);Os.mkfifo(File(home,"fifo").path,384)
        assertEquals(FileKind.LINK,listed().single { it.name=="link" }.kind)
        for(path in listOf("../outside","/outside","Alpha/../Alpha","link","link/sub")) {
            rejects { source.list(FileLocation(FileEndpoint.APP,path)) }
        }
        rejects { source.preview(FileLocation(FileEndpoint.APP,"link"),BrowserEntry("link/secret","secret",FileKind.FILE,null,null)) }
        rejects { source.preview(root(),listed().single { it.name=="fifo" }) }
        rejects { source.preview(root(),BrowserEntry("Alpha/child.txt","child.txt",FileKind.FILE,null,null)) }
        assertEquals("outside",File(outside,"secret").readText())
    }
    @Test fun localPreviewClosesDescriptorsAfterSuccessAndDecodeFailure() {
        File(home,"invalid.txt").writeBytes(byteArrayOf(0xc3.toByte(),0x28))
        val entries=listed()
        fun openPaths()=File("/proc/self/fd").listFiles()!!.mapNotNull {
            try { Os.readlink(it.path).takeIf { path -> path.contains("/${directory.name}/home/") } }
            catch (_:Exception) { null }
        }.sorted()
        val before=openPaths()
        val probe=Os.open(File(home,"notes.txt").path,OsConstants.O_RDONLY,0)
        try { assertEquals("FD observer must see an explicitly open fixture file",before.size+1,openPaths().size) }
        finally { Os.close(probe) }
        assertEquals(before,openPaths())
        repeat(3) {
            runBlocking { source.preview(root(),entries.single { it.name=="notes.txt" }) }
            rejects { source.preview(root(),entries.single { it.name=="invalid.txt" }) }
        }
        assertEquals("preview must release success and failure descriptors",before,openPaths())
    }
    @Test fun tooManyEntriesReportsFailureRatherThanPartialSuccess() {
        val many=File(home,"many").apply { mkdir() }
        repeat(LocalFileBrowser.ENTRY_LIMIT+1) { check(File(many,"$it").createNewFile()) }
        rejects { source.list(FileLocation(FileEndpoint.APP,"many")) }
    }
    @Test fun navigationAndPreviewIgnoreCancelledLateResponses() {
        val lists=mutableListOf<Continuation<List<BrowserEntry>>>()
        val previews=mutableListOf<Continuation<String>>()
        val file=BrowserEntry("note","note",FileKind.FILE,2,null)
        val fake=object:FileBrowserSource {
            override suspend fun endpoints()=listOf(FileEndpoint.APP)
            override suspend fun grantTree(uri:String,flags:Int)=error("not used")
            override suspend fun list(location:FileLocation)=suspendCoroutine<List<BrowserEntry>> { lists.add(it) }
            override suspend fun preview(location:FileLocation,entry:BrowserEntry)=suspendCoroutine<String> { previews.add(it) }
        }
        model=FilesViewModel(app,fake)
        ui.setContent { MaterialTheme { if(model.visible) FilesScreen(model,{}) } }
        ui.runOnIdle { model.enter() };ui.waitUntil { lists.size==1 }
        ui.runOnIdle { model.reload() };ui.waitUntil { lists.size==2 }
        ui.runOnIdle { lists[0].resume(listOf(file.copy(name="stale"))) }
        ui.runOnIdle { assertTrue(model.loading);assertTrue(model.entries.isEmpty()) }
        ui.runOnIdle { lists[1].resume(listOf(file)) };loaded()
        entry("note").performClick();ui.waitUntil { previews.size==1 }
        ui.onNodeWithText("返回").performClick();entry("note").performClick();ui.waitUntil { previews.size==2 }
        ui.runOnIdle { previews[1].resume("fresh") };ui.onNodeWithTag("file-preview").assertTextEquals("fresh")
        ui.runOnIdle { previews[0].resume("old") };ui.onNodeWithTag("file-preview").assertTextEquals("fresh")
        ui.runOnIdle { model.closePreview();model.reload() };ui.waitUntil { lists.size==3 }
        ui.runOnIdle { model.leave();lists[2].resume(listOf(file.copy(name="after-leave"))) }
        ui.runOnIdle { assertFalse(model.visible);assertTrue(model.entries.isEmpty()) }
    }
    @Test fun locationChangesClearSelectionAndRestoreOnlyThatEndpointsPath() {
        val other=FileEndpoint("other","Other","root","content://fixture/tree/root")
        val folder=BrowserEntry("sub","sub",FileKind.DIRECTORY,null,null)
        model=FilesViewModel(app,object:FileBrowserSource {
            override suspend fun endpoints()=listOf(FileEndpoint.APP,other)
            override suspend fun grantTree(uri:String,flags:Int)=other
            override suspend fun list(location:FileLocation)=if(location.directoryId=="") listOf(folder) else listOf(folder.copy(id="child",name="child"))
            override suspend fun preview(location:FileLocation,entry:BrowserEntry)=""
        })
        show();entry("sub").performClick();loaded();entry("child").performTouchInput { longClick() }
        ui.runOnIdle { model.choose(other) };loaded();assertTrue(model.selected.isEmpty());assertFalse(model.selecting)
        assertEquals("root",model.location.directoryId)
        ui.runOnIdle { model.choose(FileEndpoint.APP) };loaded();assertEquals("sub",model.location.directoryId)
        ui.onNodeWithText("返回").performClick();loaded();assertEquals("",model.location.directoryId)
        ui.onNodeWithText("返回").performClick();assertFalse(model.visible)
    }
}
