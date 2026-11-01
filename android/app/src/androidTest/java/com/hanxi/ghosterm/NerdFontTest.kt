package com.hanxi.ghosterm

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.os.SystemClock
import androidx.activity.ComponentActivity
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.viewinterop.AndroidView
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalSession
import com.hanxi.ghosterm.terminal.TerminalView
import org.junit.*
import org.junit.Assert.*
import java.io.File
import java.util.UUID
import kotlin.math.ceil

/** Installed font -> real PTY/VT -> production Canvas, including supplementary PUA. */
class NerdFontTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private lateinit var session: LocalSession
    private lateinit var home: File
    private lateinit var view: TerminalView
    private val symbols = listOf(0xe0b0,0xe5ff,0xe7b5,0xea60,0xf15b,0xf313,0xf418,0xf0004)
        .map { String(Character.toChars(it)) }
    @Before fun setup() {
        home=File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir,"font-${UUID.randomUUID()}").apply { mkdirs() }
        session=LocalSession(home.path)
        await { session.state().connected }
        ui.setContent { AndroidView(factory={ context -> TerminalView(context).also { view=it;it.surface=session } }) }
        ui.waitUntil(5000) { ::view.isInitialized && view.width>0 && view.height>0 }
        session.commit("stty -echo; PS1=''\r")
    }
    @After fun cleanup() {
        if(::view.isInitialized) ui.runOnIdle { view.surface=null }
        if(::session.isInitialized)session.close()
        if(::home.isInitialized)home.deleteRecursively()
    }
    private fun await(condition: ()->Boolean) {
        val end=SystemClock.elapsedRealtime()+10_000
        do { if(condition())return;SystemClock.sleep(15) } while(SystemClock.elapsedRealtime()<end)
        fail("Font/PTY condition did not arrive")
    }
    private fun paint(): Paint = TerminalView::class.java.getDeclaredField("paint").apply { isAccessible=true }.get(view) as Paint
    private fun dimension(name: String): Int = TerminalView::class.java.getDeclaredField(name).apply { isAccessible=true }.getInt(view)
    @Test fun installedRendererCoversNerdSymbolsAndRetainsSystemTextFallback() {
        ui.runOnIdle {
            val actual=paint()
            for(symbol in symbols)assertTrue("Terminal font must contain Nerd glyph U+"+symbol.codePointAt(0).toString(16),actual.hasGlyph(symbol))
            for(text in listOf("M","中","😀"))assertTrue("System text fallback: $text",actual.hasGlyph(text))
            val mono=Paint(actual).apply { typeface=Typeface.MONOSPACE }
            for(text in listOf("M","0123456789","ghosterm"))assertEquals("ASCII advance must remain monospace",mono.measureText(text),actual.measureText(text),0.01f)
            assertEquals(ceil(mono.measureText("M")).toInt(),dimension("cellWidth"))
            val license=ui.activity.assets.open("licenses/NerdFontsSymbols.txt").bufferedReader().use { it.readText() }
            assertTrue(license.contains("Nerd Fonts v3.5.1"));assertTrue(license.contains("Permission is hereby granted"))
        }
    }
    @Test fun actualPtySymbolsHaveDistinctGlyphPixelsWithoutChangingCellWidths() {
        val lines=symbols+listOf("中","😀")
        // Emit raw UTF-8 bytes; the system shell's command-line editor must not
        // interpret supplementary private-use characters before VT receives them.
        fun octal(value: String)=value.toByteArray(Charsets.UTF_8).joinToString("") { "\\%03o".format(it.toInt() and 255) }
        session.commit("printf '\\033[2J\\033[H\\033[?25l"+lines.joinToString("\\r\\n") { octal(it)+"X" }+"\\r\\nFONT_READY'\r")
        await { view.currentFrame?.line(lines.size)=="FONT_READY" }
        ui.runOnIdle {
            val frame=checkNotNull(view.currentFrame)
            val width=dimension("cellWidth");val height=dimension("cellHeight")
            val image=Bitmap.createBitmap(view.width,view.height,Bitmap.Config.ARGB_8888)
            try {
                view.draw(Canvas(image))
                val patterns=mutableSetOf<List<Int>>()
                for((row,text) in lines.withIndex()) {
                    val cell=frame.cells[row*frame.cols]
                    assertEquals(text,cell.text)
                    val columns=if(row<symbols.size)1 else 2
                    assertEquals("Terminal cell width must remain unchanged",columns,cell.width)
                    assertEquals("X",frame.cells[row*frame.cols+columns].text)
                    val pixels=(row*height until (row+1)*height).flatMap { y -> (0 until width*columns).map { x -> image.getPixel(x,y) } }
                    assertTrue("Real glyph pixels must be visible for U+"+text.codePointAt(0).toString(16),pixels.count { it!=frame.background }>3)
                    if(row<symbols.size)patterns+=pixels
                }
                assertEquals("Different Nerd icons must not all render a missing-glyph box",symbols.size,patterns.size)
                val file=File(ui.activity.cacheDir,"nerd-font-render.png")
                file.outputStream().use { image.compress(Bitmap.CompressFormat.PNG,100,it) }
            } finally { image.recycle() }
        }
    }
}
