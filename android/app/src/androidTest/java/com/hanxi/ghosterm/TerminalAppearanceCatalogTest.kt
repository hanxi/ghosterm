package com.hanxi.ghosterm

import android.app.Application
import android.graphics.Paint
import android.graphics.Typeface
import android.system.Os
import android.system.OsConstants
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import com.hanxi.ghosterm.terminal.*
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.*
import org.junit.Assert.*
import java.io.File
import java.security.MessageDigest
import java.util.UUID

class TerminalAppearanceCatalogTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var directory:File
    private lateinit var app:Application
    private lateinit var store:TerminalFontStore
    private fun font()=context.resources.openRawResource(com.hanxi.ghosterm.terminal.R.font.source_code_pro).use { it.readBytes() }
    @Before fun setup() {
        directory=File(context.cacheDir,"appearance-${UUID.randomUUID()}").apply { mkdirs() }
        app=object:Application() { init { attachBaseContext(context) };override fun getFilesDir()=directory }
        store=TerminalFontStore(app)
    }
    @After fun cleanup() { directory.deleteRecursively() }
    private fun reject(block:()->Unit) {
        var rejected=false;try { block() } catch(_:Exception) { rejected=true }
        assertTrue("The operation must reject this input",rejected)
    }
    @Test fun all725ThemesMatchThePinnedSourceIncludingAnsiCursorAndSelection() {
        val text=InstrumentationRegistry.getInstrumentation().context.assets.open("terminal-themes-reference.json").bufferedReader().use { it.readText() }
        val source=JSONObject(text).getJSONArray("themes")
        assertEquals(725,source.length());assertEquals(728,TerminalTheme.entries.size)
        for(i in 0 until source.length()) {
            val row=source.getJSONObject(i);val theme=TerminalTheme.entries.single { it.id=="ghostty:"+row.getString("name") }
            fun rgb(key:String)=row.getString(key).toInt(16)
            val appearance=AppPreferences(terminalTheme=theme).terminalStyle().appearance
            assertEquals(rgb("background"),appearance.background);assertEquals(rgb("foreground"),appearance.foreground)
            assertEquals(rgb("cursor-color"),appearance.cursorColor);assertEquals(rgb("cursor-text"),appearance.cursorText)
            assertEquals(rgb("selection-background"),appearance.selectionBackground);assertEquals(rgb("selection-foreground"),appearance.selectionForeground)
            assertEquals((0 until 16).map { row.getJSONArray("palette").getString(it).toInt(16) },appearance.palette)
        }
        val dracula=TerminalTheme.entries.single { it.label=="Dracula" }
        assertEquals(0x282a36,dracula.background);assertEquals(0xff5555,dracula.palette[1]);assertFalse(dracula.light)
        assertTrue(TerminalTheme.entries.any { it.light })
    }
    @Test fun old16PreferenceDocumentRetainsEveryValueAndUsesThePreviousSystemFont()=runBlocking {
        val disk=PreferencesFile(File(directory,"preferences"));val first=PreferencesRepository(disk)
        first.reload();first.update { it.copy(terminalTheme=TerminalTheme.FOREST,fontSize=19,keepAwake=true) }
        val file=File(directory,"preferences/preferences.json");val json=JSONObject(file.readText())
        json.getJSONObject("values").remove("fontFamily");file.writeText(json.toString());val before=file.readBytes()
        val loaded=PreferencesRepository(disk).reload() as PreferencesState.Ready
        assertEquals(TerminalFont.SYSTEM,loaded.values.fontFamily);assertEquals(19,loaded.values.fontSize)
        assertEquals(TerminalTheme.FOREST,loaded.values.terminalTheme);assertTrue(loaded.values.keepAwake)
        assertTrue(loaded.issues.isEmpty());assertArrayEquals(before,file.readBytes())
    }
    @Test fun importedFontIsAnImmutablePrivateCopyAndDuplicateImportDoesNotConsumeAnotherSlot() {
        val bytes=font();val expected=MessageDigest.getInstance("SHA-256").digest(bytes)
        val imported=store.import("../../source.ttf",bytes)
        assertEquals("source.ttf",imported.label)
        val path=store.verifiedFile(imported.id)
        assertArrayEquals(bytes,path.readBytes());assertEquals(File(directory,"fonts").canonicalFile,path.parentFile.canonicalFile)
        assertEquals(0x180,Os.stat(path.path).st_mode and 0x1ff)
        assertEquals(imported.id,store.import("duplicate.ttf",bytes).id);assertEquals(1,store.list().size)
        assertArrayEquals(expected,MessageDigest.getInstance("SHA-256").digest(font()))
        val face=TerminalFonts.prepare(app,imported.id)
        val paint=Paint().apply { typeface=face;textSize=32f }
        assertTrue(paint.hasGlyph("A"));assertTrue(paint.hasGlyph("中"));assertTrue(paint.hasGlyph("\uE0B0"))
        assertEquals(paint.measureText("i"),paint.measureText("W"),.05f)
    }
    @Test fun proportionalSymbolTruncatedCollectionAndOversizedFilesAreRejectedWithoutPublishing() {
        val symbols=context.resources.openRawResource(com.hanxi.ghosterm.terminal.R.font.nerd_symbols_mono).use { it.readBytes() }
        val proportional=File("/system/fonts/Roboto-Regular.ttf").readBytes()
        val collection=font().apply { "ttcf".toByteArray().copyInto(this) }
        val malformed=font().apply { this[4]=127;this[5]=127 }
        listOf(ByteArray(0),font().copyOf(100),symbols,proportional,collection,malformed,ByteArray(TerminalFontStore.MAX_FONT_BYTES+1)).forEach {
            reject { store.import("invalid.ttf",it) }
        }
        assertTrue(store.list().isEmpty());assertFalse(File(directory,"fonts").exists())
    }
    @Test fun warmTypefaceCacheCannotHideChangedOrMissingPrivateFont() {
        val entry=store.import("checked.ttf",font());TerminalFonts.prepare(app,entry.id)
        val file=store.verifiedFile(entry.id);file.appendBytes(byteArrayOf(1))
        reject { TerminalFonts.prepare(app,entry.id) }
        assertNotNull(store.list().single().problem)
        assertTrue(file.delete());reject { TerminalFonts.prepare(app,entry.id) }
    }
    @Test fun fontDirectoryAndFontFileSymlinksAreRejected() {
        val other=File(directory,"other").apply { mkdir() };val fonts=File(directory,"fonts")
        Os.symlink(other.path,fonts.path);reject { store.import("link.ttf",font()) }
        assertTrue(other.listFiles()!!.isEmpty());assertTrue(fonts.delete())
        val entry=store.import("ordinary.ttf",font());val target=store.verifiedFile(entry.id)
        val original=File(other,"keep.ttf").apply { writeBytes(font()) };assertTrue(target.delete());Os.symlink(original.path,target.path)
        reject { store.verifiedFile(entry.id) };assertArrayEquals(font(),original.readBytes())
    }
    @Test fun importCancellationBeforePublicationCleansItsStages() {
        var checks=0
        reject { store.import("cancel.ttf",font()) { if(++checks==2) throw CancellationException("cancel before publish") } }
        assertTrue(store.list().isEmpty());assertTrue(File(directory,"fonts").listFiles().orEmpty().isEmpty())
    }
    @Test fun quotaIncludesResidualFilesAndDoesNotLeaveAnotherStage() {
        val fonts=File(directory,"fonts").apply { mkdir() }
        val hold=File(fonts,".reserved");java.io.RandomAccessFile(hold,"rw").use { it.setLength(TerminalFontStore.MAX_TOTAL_BYTES) }
        reject { store.import("quota.ttf",font()) }
        assertEquals(listOf(".reserved"),fonts.listFiles()!!.map { it.name })
        // A font fitting exactly still needs space for its name record.
        java.io.RandomAccessFile(hold,"rw").use { it.setLength(TerminalFontStore.MAX_TOTAL_BYTES-font().size) }
        reject { store.import("metadata-budget.ttf",font()) }
        assertEquals(listOf(".reserved"),fonts.listFiles()!!.map { it.name })
    }
    @Test fun fontSelectionAndDeletionShareOneSerializedOwnerAndProtectTheSelectedFont()=runBlocking {
        val entry=store.import("owned.ttf",font());val value=checkNotNull(TerminalFont.fromId(entry.id))
        val owner=AppSettings(PreferencesRepository(PreferencesFile(File(directory,"preferences"))),fontApplication=app)
        try {
            withTimeout(10000) { while(owner.state.value.storage !is PreferencesState.Ready) delay(10) }
            assertTrue(owner.save(UUID.randomUUID(),listOf(PreferenceEdit.FontFamily(value))) is PreferenceUpdate.Saved)
            assertNotNull(owner.removeFont(UUID.randomUUID(),entry.id));assertTrue(store.verifiedFile(entry.id).exists())
            assertTrue(owner.save(UUID.randomUUID(),listOf(PreferenceEdit.FontFamily(TerminalFont.SYSTEM))) is PreferenceUpdate.Saved)
            assertNull(owner.removeFont(UUID.randomUUID(),entry.id))
            val before=File(directory,"preferences/preferences.json").readBytes()
            assertTrue(owner.save(UUID.randomUUID(),listOf(PreferenceEdit.FontFamily(value))) is PreferenceUpdate.Failed)
            assertEquals(TerminalFont.SYSTEM,owner.state.value.committed.fontFamily)
            assertArrayEquals(before,File(directory,"preferences/preferences.json").readBytes())
        } finally { owner.close();owner.awaitClosed() }
    }
}
