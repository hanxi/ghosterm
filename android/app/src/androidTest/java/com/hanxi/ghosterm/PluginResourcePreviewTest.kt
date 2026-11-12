package com.hanxi.ghosterm

import android.graphics.Bitmap
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.ByteArrayOutputStream

@RunWith(AndroidJUnit4::class)
class PluginResourcePreviewTest {
    @Test fun textPreviewRequiresUtf8AndCapsRenderedCharacters() {
        assertEquals("Markdown **source**", decodePluginResourceText("Markdown **source**".toByteArray()))
        assertNull(decodePluginResourceText(byteArrayOf(0xc3.toByte(), 0x28)))
        assertEquals(8192, decodePluginResourceText(ByteArray(9000) { 'x'.code.toByte() })?.length)
        assertNull(decodePluginResourceText(ByteArray(1048577)))
    }

    @Test fun imagePreviewDecodesSmallRasterAndRejectsOversizePayload() {
        val source = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888).apply {
            eraseColor(android.graphics.Color.RED)
        }
        val bytes = ByteArrayOutputStream().use { output ->
            assertTrue(source.compress(Bitmap.CompressFormat.PNG, 100, output))
            output.toByteArray()
        }
        val preview = decodePluginResourceBitmap(bytes)
        assertNotNull(preview)
        assertEquals(android.graphics.Color.RED, preview!!.getPixel(0, 0))
        assertNull(decodePluginResourceBitmap(ByteArray(1048577)))
        source.recycle()
        preview.recycle()
    }
}
