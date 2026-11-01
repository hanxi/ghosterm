package com.hanxi.ghosterm

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.os.SystemClock
import androidx.activity.ComponentActivity
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.viewinterop.AndroidView
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalSession
import com.hanxi.ghosterm.bridge.NativeTerminalSession
import com.hanxi.ghosterm.bridge.RemoteSession
import com.hanxi.ghosterm.bridge.ResolvedSSHConnection
import com.hanxi.ghosterm.bridge.SSHAuthPhase
import com.hanxi.ghosterm.terminal.TerminalView
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Actual PTY -> native VT owner -> immutable Bitmap -> production Canvas View. */
@RunWith(AndroidJUnit4::class)
class KittyCanvasTest {
    @get:Rule val ui=createAndroidComposeRule<ComponentActivity>()
    private lateinit var home: File
    private lateinit var session: LocalSession
    private lateinit var view: TerminalView
    private lateinit var capture: Bitmap
    private var error: String?=null
    @Before fun setup() {
        home=File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir,"kitty-${UUID.randomUUID()}").apply { mkdirs() }
        session=LocalSession(home.absolutePath)
        await { session.state().connected }
        capture=Bitmap.createBitmap(256,256,Bitmap.Config.ARGB_8888)
        ui.setContent { AndroidView(factory={ context -> TerminalView(context).also {
            view=it;it.surface=session;it.onError={ value -> error=value }
        } }) }
        ui.waitUntil(5000) { ::view.isInitialized && view.width>0 && view.height>0 }
        session.commit("stty -echo; PS1=''\r")
    }
    @After fun cleanup() {
        if(::view.isInitialized)ui.runOnIdle { view.surface=null }
        if(::session.isInitialized)session.close()
        val type=Class.forName("com.hanxi.ghosterm.bridge.NativeSession")
        await(checkState=false) { type.getDeclaredMethod("ownerCount").invoke(type.getField("INSTANCE").get(null))==0 && metrics().drop(1).all { it==0L } }
        assertEquals("bitmap caches released",0L,metrics()[1]);assertEquals("native frame refs released",0L,metrics()[2])
        if(::capture.isInitialized)capture.recycle()
        if(::home.isInitialized)home.deleteRecursively()
    }
    private fun await(checkState: Boolean=true,condition: ()->Boolean) {
        val end=SystemClock.elapsedRealtime()+10000
        while(SystemClock.elapsedRealtime()<end) {
            if(condition())return
            if(checkState) { assertNull(error);assertFalse(session.state().description,session.state().terminal) }
            SystemClock.sleep(20)
        }
        fail("Kitty Canvas condition timed out: ${session.state()}; UI=$error; placements=${session.graphics(0)?.placements}; files=${home.listFiles()?.map { it.name+":"+it.length() }}; text=${session.frame(0)?.cells?.joinToString("") { it.text }}")
    }
    private fun metrics(): LongArray {
        val type=Class.forName("com.hanxi.ghosterm.bridge.NativeSession")
        return type.getDeclaredMethod("readGraphicsMetrics").invoke(type.getField("INSTANCE").get(null)) as LongArray
    }
    private fun redPixels()=ui.runOnIdle {
        capture.eraseColor(Color.TRANSPARENT);view.draw(Canvas(capture))
        val pixels=IntArray(capture.width*capture.height)
        capture.getPixels(pixels,0,capture.width,0,0,capture.width,capture.height)
        pixels.count { it==Color.RED }
    }
    private fun output(value: String,target: NativeTerminalSession=session) {
        val escaped=value.toByteArray().joinToString("") { "\\%03o".format(it.toInt() and 255) }
        target.commit("printf '$escaped'\r")
    }
    private fun pixel()=ui.runOnIdle {
        capture.eraseColor(Color.TRANSPARENT)
        view.draw(Canvas(capture));capture.getPixel(3,3)
    }
    @Test fun remoteSshUsesTheSameCanvasAndAnimationOwner() {
        val fixture=sshFixture()
        val host="kitty-fixture.ghosterm.test"
        val known=File(home,"known_hosts").apply { writeText("[$host]:${fixture.getInt("port")} "+fixture.getString("public")) }
        val remote=RemoteSession(ResolvedSSHConnection(host,fixture.getInt("port"),fixture.getString("user"),known.path,byteArrayOf(127,0,0,1)))
        try {
            await {
                val phase=remote.authentication()
                assertNotEquals(phase.description,SSHAuthPhase.FAILED,phase.phase)
                phase.phase==SSHAuthPhase.CREDENTIALS && phase.request>0
            }
            val key=fixture.getString("pem").toByteArray()
            remote.privateKey(remote.authentication(),key)
            assertTrue(key.all { it==0.toByte() })
            await { assertFalse(remote.state().description,remote.state().terminal);remote.state().connected }
            ui.runOnIdle { view.surface=remote }
            remote.commit("stty -echo; PS1=''\r")
            output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=10,c=4,r=3,q=2;/wAA/w==\u001b\\"+
                "\u001b_Ga=f,f=32,s=1,v=1,i=10,z=70,q=2;AP8A/w==\u001b\\"+
                "\u001b_Ga=a,i=10,r=1,z=70,q=2\u001b\\"+
                "\u001b_Ga=a,i=10,s=3,v=1,q=2\u001b\\",remote)
            await { assertFalse(remote.state().description,remote.state().terminal);pixel()==Color.RED }
            await { pixel()==Color.GREEN };await { pixel()==Color.RED }
            assertTrue(remote.graphics(0)!!.placements>0)
        } finally { remote.close() }
    }
    @Test fun actualViewPaintsImageAndClearsDeletedPlacement() {
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=1,c=4,r=3,q=2;/wAA/w==\u001b\\")
        await { pixel()==Color.RED }
        assertTrue(session.graphics(0)!!.placements>0)
        output("\u001b_Ga=d,d=A,q=2\u001b\\")
        await { session.graphics(0)?.placements==0 && pixel()!=Color.RED }
        assertTrue(session.state().connected)
    }
    @Test fun nativeOwnerDecodesPngAndCanvasCompositesAlpha() {
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=100,i=3,c=4,r=3,q=2;iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNwUepoAAADCwFv4Tw4tgAAAABJRU5ErkJggg==\u001b\\")
        val background=session.frame(0)!!.background
        fun blend(value: Int,base: Int)=(value*128+base*127+127)/255
        val red=blend(68,Color.red(background));val green=blend(34,Color.green(background));val blue=blend(136,Color.blue(background))
        await { val actual=pixel();kotlin.math.abs(Color.red(actual)-red)<=2 && kotlin.math.abs(Color.green(actual)-green)<=2 && kotlin.math.abs(Color.blue(actual)-blue)<=2 }
        assertTrue(session.graphics(0)!!.placements>0)
    }
    @Test fun belowBackgroundImageRespectsExplicitBackgroundEvenWhenItMatchesDefault() {
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=4,c=4,r=3,z=-2147483648,q=2;/wAA/w==\u001b\\")
        await { pixel()==Color.RED }
        val background=session.frame(0)!!.background
        output("\u001b[H\u001b[48;2;${Color.red(background)};${Color.green(background)};${Color.blue(background)}m \u001b[0m")
        await { pixel()==background }
        val remaining=ui.runOnIdle { (0 until capture.width).count { capture.getPixel(it,3)==Color.RED } }
        assertTrue("default background cells retain below-background image",remaining>0)
    }
    @Test fun unicodePlaceholderUsesUpstreamGeometryInsteadOfPaintingATextGlyph() {
        val placeholder=String(Character.toChars(0x10eeee))+"\u0305\u0305"
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=6,p=42,U=1,c=1,r=1,q=2;/wAA/w==\u001b\\"+
            "\u001b[H\u001b[38;2;0;0;6m"+placeholder+"\u001b[0m")
        await { redPixels()>2 }
        assertTrue("virtual fragments reach Canvas",session.graphics(0)!!.placements>0)
    }
    @Test fun tinyUnicodeImageCanSpanMultipleCellsWithoutLosingFragments() {
        val mark=String(Character.toChars(0x10eeee))
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=11,p=43,U=1,c=4,r=1,q=2;/wAA/w==\u001b\\"+
            "\u001b[H\u001b[38;2;0;0;11m"+mark+"\u0305\u0305"+mark+"\u0305\u030d"+mark+"\u0305\u030e"+mark+"\u0305\u0310\u001b[0m")
        await { (session.graphics(0)?.placements ?: 0)>0 && redPixels()>20 }
        assertTrue(session.state().connected)
    }
    @Test fun zlibCompressedRgbDataCanArriveInSeparateProtocolChunks() {
        val zip=java.util.zip.Deflater()
        val compressed=try {
            zip.setInput(byteArrayOf(0,255.toByte(),0));zip.finish()
            ByteArray(128).let { it.copyOf(zip.deflate(it)) }
        } finally { zip.end() }
        val data=android.util.Base64.encodeToString(compressed,android.util.Base64.NO_WRAP)
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=24,s=1,v=1,i=12,c=4,r=3,o=z,m=1,q=2;${data.take(4)}\u001b\\")
        output("\u001b[H\u001b_Gm=0,q=2;${data.drop(4)}\u001b\\")
        await { pixel()==Color.GREEN }
    }
    @Test fun alternateScreenSeparatesGraphicsAndRestoresTheMainImage() {
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=13,c=4,r=3,q=2;/wAA/w==\u001b\\")
        await { pixel()==Color.RED }
        output("\u001b[?1049h\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=13,c=4,r=3,q=2;AP8A/w==\u001b\\")
        await { pixel()==Color.GREEN }
        output("\u001b[?1049l")
        await { pixel()==Color.RED }
        output("\u001b_Ga=d,d=A,q=2\u001b\\")
        await { session.graphics(0)?.placements==0 && pixel()!=Color.RED }
    }
    @Test fun kittyQueryResponseReturnsThroughTheActualPty() {
        val expected="\u001b_Gi=33;OK\u001b\\".toByteArray()
        val file=File(home,"kitty-response")
        session.commit("stty icanon echo; printf '\\033_Gi=33,s=1,v=1,a=q,t=d,f=24;AAAA\\033\\134'; dd bs=1 count=${expected.size} of='${file.absolutePath}' 2>/dev/null; stty sane\r")
        // Canonical input is deliberate: observe the actual reply echoed by
        // the PTY, then release the line to dd. API 29 toybox stty cannot apply
        // all raw flags on this emulator; graphics transport is independent.
        await { session.frame(0)?.cells?.joinToString("") { it.text }?.contains("Gi=33;OK")==true }
        session.commit("\n")
        await { file.exists() && file.length()==expected.size.toLong() }
        assertArrayEquals(expected,file.readBytes())
        assertTrue(session.state().connected)
    }
    @Test fun excessivePlacementDimensionsAreRejectedWithoutDisconnectingTheTerminal() {
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=18,p=1,c=4,r=3,q=2;/wAA/w==\u001b\\"+
            "\u001b_Ga=p,i=18,p=99,c=4294967295,r=1,C=1,q=2\u001b\\"+
            "\u001b[HBOUND_READY")
        await { session.frame(0)?.cells?.joinToString("") { it.text }?.contains("BOUND_READY")==true && pixel()==Color.RED }
        await { session.graphics(0)?.placements==1 }
        assertTrue(session.state().connected)
    }
    @Test fun fontResizeUpdatesExistingImageGeometryWithoutRetransmission() {
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=16,c=4,r=3,q=2;/wAA/w==\u001b\\")
        await { pixel()==Color.RED }
        val before=redPixels();val revision=session.graphics(0)!!.revision
        ui.runOnIdle { view.setFontSize(20f) }
        await { session.graphics(0)!!.revision>revision && redPixels()>before*3/2 }
        assertEquals(Color.RED,pixel())
    }
    @Test fun imageScrollsIntoHistoryAndReappearsWhenTheViewportMovesBack() {
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=17,c=4,r=3,q=2;/wAA/w==\u001b\\")
        await { pixel()==Color.RED }
        output("\u001b[H"+"\r\n".repeat(session.frame(0)!!.rows+20))
        await { session.graphics(0)?.placements==0 }
        var found=false
        repeat(30) {
            if(!found) { session.scroll(-5);SystemClock.sleep(75);found=redPixels()>5 }
        }
        assertTrue("scrollback must retain image pixels",found)
    }
    @Test fun relativeChildUsesItsVirtualParentAndDisappearsWithoutVisiblePlaceholders() {
        val placeholder=String(Character.toChars(0x10eeee))+"\u0305\u0305"
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=14,p=42,U=1,c=1,r=1,q=2;/wAA/w==\u001b\\"+
            "\u001b_Ga=T,f=32,s=1,v=1,i=15,p=1,P=14,Q=42,H=1,V=0,c=1,r=1,q=2;AP8A/w==\u001b\\"+
            "\u001b[H\u001b[38;2;0;0;14m"+placeholder+"\u001b[0m\u001b[?25l")
        await { redPixels()>2 && (session.graphics(0)?.placements ?: 0)>=2 }
        fun childPixels()=ui.runOnIdle {
            capture.eraseColor(Color.TRANSPARENT);view.draw(Canvas(capture))
            val pixels=IntArray(capture.width*capture.height)
            capture.getPixels(pixels,0,capture.width,0,0,capture.width,capture.height)
            pixels.count { it==Color.GREEN }
        }
        await { childPixels()>2 }
        output("\u001b[2J\u001b[H")
        await { session.graphics(0)?.placements==0 }
    }
    @Test fun manualFrameSelectionAndCompositionUpdateTheDisplayedImage() {
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=7,c=4,r=3,q=2;/wAA/w==\u001b\\"+
            "\u001b_Ga=f,f=32,s=1,v=1,i=7,z=70,q=2;AP8A/w==\u001b\\")
        await { pixel()==Color.RED }
        output("\u001b_Ga=a,i=7,c=2,s=1,q=2\u001b\\")
        await { pixel()==Color.GREEN }
        val stopped=session.graphics(0)!!.revision
        SystemClock.sleep(200)
        assertEquals(Color.GREEN,pixel());assertEquals(stopped,session.graphics(0)!!.revision)
        output("\u001b_Ga=c,i=7,r=1,c=2,C=1,q=2\u001b\\")
        await { pixel()==Color.RED }
    }
    @Test fun finiteAnimationParksOnItsLastFrame() {
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=8,c=4,r=3,q=2;/wAA/w==\u001b\\"+
            "\u001b_Ga=f,f=32,s=1,v=1,i=8,z=70,q=2;AP8A/w==\u001b\\"+
            "\u001b_Ga=a,i=8,r=1,z=70,q=2\u001b\\"+
            "\u001b_Ga=a,i=8,s=3,v=2,q=2\u001b\\")
        await { pixel()==Color.GREEN }
        val parked=session.graphics(0)!!.revision
        SystemClock.sleep(300)
        assertEquals(Color.GREEN,pixel());assertEquals(parked,session.graphics(0)!!.revision)
    }
    @Test fun hiddenViewStopsAnimationClockAndResumesTheSameImage() {
        output("\u001b[2J\u001b[H\u001b_Ga=T,f=32,s=1,v=1,i=9,c=4,r=3,q=2;/wAA/w==\u001b\\"+
            "\u001b_Ga=f,f=32,s=1,v=1,i=9,z=70,q=2;AP8A/w==\u001b\\"+
            "\u001b_Ga=a,i=9,r=1,z=70,q=2\u001b\\"+
            "\u001b_Ga=a,i=9,s=3,v=1,q=2\u001b\\")
        await { pixel()==Color.GREEN }
        ui.runOnIdle { view.visibility=android.view.View.GONE }
        SystemClock.sleep(100)
        val hidden=metrics()[0]
        SystemClock.sleep(250)
        assertEquals("hidden animation clock must not wake",hidden,metrics()[0])
        ui.runOnIdle { view.visibility=android.view.View.VISIBLE }
        await { metrics()[0]>hidden }
        await { pixel()==Color.RED };await { pixel()==Color.GREEN }
    }
    @Test fun animationRepaintsWithoutNewOutputOrRebuildingTextSnapshots() {
        output("\u001b[2J\u001b[H"+
            "\u001b_Ga=T,f=32,s=1,v=1,i=2,c=4,r=3,q=2;/wAA/w==\u001b\\"+
            "\u001b_Ga=f,f=32,s=1,v=1,i=2,z=70,q=2;AP8A/w==\u001b\\"+
            "\u001b_Ga=a,i=2,r=1,z=70,q=2\u001b\\"+
            "\u001b_Ga=a,i=2,s=3,v=1,q=2\u001b\\")
        await { pixel()==Color.RED }
        val text=session.frame(0)!!.revision
        await { pixel()==Color.GREEN };await { pixel()==Color.RED };await { pixel()==Color.GREEN }
        assertEquals("image clock does not rebuild text grid",text,session.frame(0)!!.revision)
    }
}
