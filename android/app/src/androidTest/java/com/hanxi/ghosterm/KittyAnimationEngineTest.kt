package com.hanxi.ghosterm

import android.graphics.Color
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.terminal.TerminalEngine
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/** Real upstream parser/frame storage/clock; Canvas rendering is separate. */
@RunWith(AndroidJUnit4::class)
class KittyAnimationEngineTest {
    @Test fun platformPngDecoderPreservesStraightAlpha() {
        val input="\u001b_Ga=T,f=100,i=42;iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNwUepoAAADCwFv4Tw4tgAAAABJRU5ErkJggg==\u001b\\"
        val frame=TerminalEngine.probeKittyFrames(input.toByteArray(),42,longArrayOf(0))
        assertEquals(0x80442288.toInt(),frame[2])
        assertEquals(-1,frame[1])
    }
    @Test fun excessiveDimensionsAreRejectedAndAValidImageStillLoads() {
        val green=android.util.Base64.encodeToString(ByteArray(4097*4) { index -> if(index%4==1 || index%4==3) 255.toByte() else 0 },android.util.Base64.NO_WRAP)
        val invalid="\u001b_Ga=t,f=32,s=4097,v=1,i=43;$green\u001b\\"
        try {
            TerminalEngine.probeKittyFrames(invalid.toByteArray(),43,longArrayOf(0))
            fail("oversized image must be rejected")
        } catch(expected: IllegalStateException) { assertEquals("Kitty frame/clock probe failed",expected.message) }
        val input=invalid+"\u001b_Ga=T,f=32,s=1,v=1,i=45;/wAA/w==\u001b\\"
        assertEquals(Color.RED,TerminalEngine.probeKittyFrames(input.toByteArray(),45,longArrayOf(0))[2])
    }
    @Test fun frameMetadataLimitRejectsAnExtraFrameAndLeavesPlaybackUsable() {
        val input=buildString {
            append("\u001b_Ga=T,f=32,s=1,v=1,i=44;/wAA/w==\u001b\\")
            repeat(1023) { append("\u001b_Ga=f,f=32,s=1,v=1,i=44;AP8A/w==\u001b\\") }
            append("\u001b_Ga=a,i=44,c=1024,s=1\u001b\\")
            append("\u001b_Ga=f,f=32,s=1,v=1,i=44;AAD//w==\u001b\\")
            append("\u001b_Ga=a,i=44,c=1025,s=1\u001b\\")
        }
        val frame=TerminalEngine.probeKittyFrames(input.toByteArray(),44,longArrayOf(0))
        assertEquals("extra frame must be rejected without damaging prior frame",Color.GREEN,frame[2])
    }
    @Test fun rgbaFramesAdvanceWithoutAdditionalTerminalBytes() {
        val input="\u001b_Ga=T,f=32,s=1,v=1,i=41;/wAA/w==\u001b\\"+
            "\u001b_Ga=f,f=32,s=1,v=1,i=41,z=50;AP8A/w==\u001b\\"+
            "\u001b_Ga=a,i=41,r=1,z=50;\u001b\\"+
            "\u001b_Ga=a,i=41,s=3,v=1;\u001b\\"
        val frames=TerminalEngine.probeKittyFrames(input.toByteArray(),41,longArrayOf(1000,1049,1050,1100))
        assertEquals(16,frames.size)
        assertEquals(0xffff0000.toInt(),frames[2]);assertEquals(0xffff0000.toInt(),frames[6])
        assertEquals(0xff00ff00.toInt(),frames[10]);assertEquals(0xffff0000.toInt(),frames[14])
        assertEquals(50,frames[1]);assertEquals(1,frames[5]);assertEquals(1,frames[8])
        assertNotEquals(frames[3],frames[11])
    }
}
