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
import com.hanxi.ghosterm.terminal.TerminalCell
import com.hanxi.ghosterm.terminal.TerminalFrame
import com.hanxi.ghosterm.terminal.TerminalFonts
import com.hanxi.ghosterm.terminal.TerminalStyle
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
    private fun drawUnclipped(canvas: Canvas, cell: TerminalCell, x: Float, y: Float) {
        TerminalView::class.java.getDeclaredMethod("drawCellText", Canvas::class.java,
            TerminalCell::class.java, Float::class.javaPrimitiveType, Float::class.javaPrimitiveType)
            .apply { isAccessible=true }.invoke(view,canvas,cell,x,y)
    }
    private val promptSymbols = listOf("➜", "→", "❯", "❮", "⑂", "⚠", "⚠️", "⚠︎", "❗", "‼", "✓", "✗", "⎇", "\ue0a0")
    private val emojiSymbols = listOf("😀", "🚀", "🔥", "✅", "❤️", "👍🏽", "👩‍💻", "🇨🇳", "1️⃣")
    @Test fun privateUseInkFitsWithoutRelyingOnCellClipping() = assertInkFits(
        symbols+listOf(0xf489,0xebc5,0xe0b2).map { String(Character.toChars(it)) }, listOf(1))
    @Test fun promptSymbolsFitWithoutClipping() = assertInkFits(promptSymbols, listOf(1,2))
    @Test fun colorEmojiFitWithoutClipping() = assertInkFits(emojiSymbols, listOf(1,2), requireColor=true)
    @Test fun roundedPowerlineCapsJoinStatusBackground() {
        for(face in listOf("system","jetbrains_mono","source_code_pro")) {
            TerminalFonts.prepare(ui.activity,face)
            ui.runOnIdle {
                view.surface=null
                for(size in listOf(8f,12f,24f,32f)) for(line in listOf(1f,1.85f)) {
                    view.updateStyle(TerminalStyle(fontId=face,fontSize=size,lineHeight=line))
                    val width=dimension("cellWidth");val height=dimension("cellHeight")
                    val image=Bitmap.createBitmap(width+8,height+8,Bitmap.Config.ARGB_8888)
                    try {
                        for(icon in listOf("\ue0b4","\ue0b6")) for(flags in listOf(0,1,2,3)) {
                            image.eraseColor(Color.TRANSPARENT)
                            paint().apply { color=Color.WHITE;isFakeBoldText=flags and 1!=0;textSkewX=if(flags and 2!=0) -.2f else 0f }
                            drawUnclipped(Canvas(image),TerminalCell(Color.BLACK,Color.WHITE,flags,1,icon),4f,4f)
                            val edge=if(icon=="\ue0b4") 4 else 4+width-1
                            val label="$face $size/$line $icon flags=$flags"
                            for(y in 1 until height-1) assertTrue("Full-height joining edge: $label y=$y",
                                Color.alpha(image.getPixel(edge,y+4))>200)
                            assertTrue("Rounded outer corner: $label",Color.alpha(image.getPixel(
                                if(icon=="\ue0b4") 4+width-1 else 4,4))<32)
                            for(y in 0 until image.height) for(x in 0 until image.width) {
                                if(x !in 4 until width+4 || y !in 4 until height+4)
                                    assertEquals("No adjacent-cell spill: $label",0,Color.alpha(image.getPixel(x,y)))
                            }
                        }
                    } finally { image.recycle() }
                }
            }
        }
    }
    private fun assertInkFits(icons: List<String>, columns: List<Int>, requireColor: Boolean = false) {
        for(face in listOf("system","jetbrains_mono","source_code_pro")) {
            TerminalFonts.prepare(ui.activity,face)
            ui.runOnIdle {
                view.surface=null
                for(size in listOf(8f,12f,24f,32f)) for(line in listOf(1f,1.85f)) for(cols in columns) {
                    view.updateStyle(TerminalStyle(fontId=face,fontSize=size,lineHeight=line))
                    val width=dimension("cellWidth")*cols;val height=dimension("cellHeight")
                    val margin=256
                    val image=Bitmap.createBitmap(width+margin*2,height+margin*2,Bitmap.Config.ARGB_8888)
                    try {
                        val canvas=Canvas(image)
                        for(flags in listOf(0,1,2,3)) for(icon in icons) {
                            image.eraseColor(Color.TRANSPARENT)
                            paint().apply { color=Color.WHITE;isFakeBoldText=flags and 1!=0;textSkewX=if(flags and 2!=0) -.2f else 0f;isUnderlineText=false;isStrikeThruText=false }
                            drawUnclipped(canvas,TerminalCell(Color.BLACK,Color.WHITE,flags,cols,icon),margin.toFloat(),margin.toFloat())
                            var ink=0;var outside=0
                            val pixels=IntArray(image.width*image.height)
                            image.getPixels(pixels,0,image.width,0,0,image.width,image.height)
                            for(index in pixels.indices) if(Color.alpha(pixels[index])>8) {
                                ink++
                                if(index%image.width !in margin until margin+width || index/image.width !in margin until margin+height) outside++
                            }
                            val label="$face $size/$line flags=$flags cols=$cols U+${icon.codePointAt(0).toString(16)}"
                            assertTrue("Visible complete icon: $label",ink>3)
                            if(requireColor) assertTrue("Color emoji retains its colored ink: $label",pixels.count {
                                Color.alpha(it)>8 && maxOf(Color.red(it),Color.green(it),Color.blue(it))-
                                    minOf(Color.red(it),Color.green(it),Color.blue(it))>20
                            }>3)
                            if(outside!=0) {
                                val bounds=android.graphics.Rect()
                                paint().getTextBounds(icon,0,icon.length,bounds)
                                val inkIndices=pixels.indices.filter { Color.alpha(pixels[it])>8 }
                                val inkBox=listOf(inkIndices.minOf { it%image.width }-margin,inkIndices.minOf { it/image.width }-margin,
                                    inkIndices.maxOf { it%image.width }-margin,inkIndices.maxOf { it/image.width }-margin)
                                fail("Unclipped glyph must fit its cell: $label grid=$width/$height bounds=$bounds ink=$inkBox outside=$outside")
                            }
                        }
                    } finally { image.recycle() }
                }
            }
        }
    }
    @Test fun normalAndBlockCursorUseTheCompleteFittedGlyph() {
        ui.runOnIdle {
            view.surface=null
            view.updateStyle(TerminalStyle(fontSize=24f,lineHeight=1f,
                appearance=view.style.appearance.copy(blink=false,cursorText=0xffffff)))
            view.requestFocus()
            assertTrue("Fixture: focused renderer",view.hasFocus() && view.hasWindowFocus())
            for(icon in listOf("\uf489","\ue0b4","\ue0b6")+promptSymbols+emojiSymbols) for(cols in listOf(1,2)) {
                val width=dimension("cellWidth")*cols;val height=dimension("cellHeight")
                val cell=TerminalCell(Color.BLACK,Color.WHITE,0,cols,icon)
                val expected=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888)
                val actual=Bitmap.createBitmap(view.width,view.height,Bitmap.Config.ARGB_8888)
                try {
                    expected.eraseColor(Color.BLACK)
                    paint().apply { color=Color.WHITE;isFakeBoldText=false;textSkewX=0f;isUnderlineText=false;isStrikeThruText=false }
                    drawUnclipped(Canvas(expected),cell,0f,0f)
                    for(cursor in listOf(false,true)) {
                        val cells=listOf(cell)+(if(cols==2) listOf(TerminalCell(Color.BLACK,Color.WHITE,0,0,"")) else emptyList())+
                            List(cols) { TerminalCell(Color.BLACK,Color.WHITE,0,1,"") }
                        val frame=TerminalFrame(1,cols*2,1,Color.BLACK,Color.WHITE,0,0,cursor,false,1,cells,Color.BLACK)
                        TerminalView::class.java.getDeclaredField("currentFrame").apply { isAccessible=true }.set(view,frame)
                        view.draw(Canvas(actual))
                        for(y in 0 until height) for(x in 0 until width)
                            assertEquals("Complete production glyph, cursor=$cursor at $x,$y",expected.getPixel(x,y),actual.getPixel(x,y))
                        for(y in 0 until height) for(x in width until width*2)
                            assertEquals("Glyph must not spill into adjacent cell",Color.BLACK,actual.getPixel(x,y))
                    }
                } finally { expected.recycle();actual.recycle() }
            }
        }
    }
    @Test fun textBaselineAndJoiningEdgesRemainUnchanged() {
        ui.runOnIdle {
            view.surface=null
            view.updateStyle(TerminalStyle(fontSize=24f,lineHeight=1f))
            val baseline=TerminalView::class.java.getDeclaredField("baseline").apply { isAccessible=true }.getFloat(view)
            val expected=Bitmap.createBitmap(512,512,Bitmap.Config.ARGB_8888)
            val actual=Bitmap.createBitmap(512,512,Bitmap.Config.ARGB_8888)
            try {
                for(text in listOf("M", "$", "+", "~", "中", "é", "│", "─", "█", "▄")) {
                    expected.eraseColor(Color.TRANSPARENT);actual.eraseColor(Color.TRANSPARENT)
                    paint().apply { color=Color.WHITE;isFakeBoldText=false;textSkewX=0f;isUnderlineText=false;isStrikeThruText=false }
                    Canvas(expected).drawText(text,100f,100f+baseline,paint())
                    drawUnclipped(Canvas(actual),TerminalCell(Color.BLACK,Color.WHITE,0,2,text),100f,100f)
                    assertTrue("Original baseline and joining edges: $text",expected.sameAs(actual))
                }
            } finally { expected.recycle();actual.recycle() }
        }
    }
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
    @Test fun actualPtySymbolsHaveDistinctGlyphPixelsWithoutChangingCellWidths() = assertPtySymbols(false)
    @Test fun graphemePtySymbolsRetainTheirAllocatedColumns() = assertPtySymbols(true)
    private fun assertPtySymbols(clustered: Boolean) {
        // Legacy VT keeps VS16 warning in one column; mode 2027 makes it two.
        // The renderer must honor either allocation, without changing cursor positions.
        val lines=symbols+listOf("中","😀","➜","→","❯","⑂","⚠","⚠️","❗","\ue0a0")+
            if(clustered) listOf("👩‍💻","1️⃣") else emptyList()
        val widths=List(symbols.size) { 1 }+listOf(2,2,1,1,1,1,1,if(clustered)2 else 1,2,1)+
            if(clustered) listOf(2,2) else emptyList()
        // Emit raw UTF-8 bytes; the system shell's command-line editor must not
        // interpret supplementary private-use characters before VT receives them.
        fun octal(value: String)=value.toByteArray(Charsets.UTF_8).joinToString("") { "\\%03o".format(it.toInt() and 255) }
        session.commit("printf '\\033[?2027${if(clustered) "h" else "l"}\\033[2J\\033[H\\033[?25l"+lines.joinToString("\\r\\n") { octal(it)+"X" }+"\\r\\nFONT_READY'\r")
        await { view.currentFrame?.let { it.rows>lines.size && it.line(lines.size)=="FONT_READY" }==true }
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
                    val columns=widths[row]
                    assertEquals("Terminal cell width must remain unchanged: $text row=$row",columns,cell.width)
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
