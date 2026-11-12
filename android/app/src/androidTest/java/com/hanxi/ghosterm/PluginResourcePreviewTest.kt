package com.hanxi.ghosterm

import android.graphics.Bitmap
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.ByteArrayOutputStream

@RunWith(AndroidJUnit4::class)
class PluginResourcePreviewTest {
    @Test fun markdownIsFormattedWithoutLinksImagesOrRawHtml() {
        val decoded = PluginResourceMarkdownDecoder.decodeDocument(
            "# Title\n\nA **bold** [link](https://example.com) ![diagram](https://example.com/a.png)\n\n<script>alert(1)</script>".toByteArray())
        assertNotNull(decoded)
        val rendered = decoded!!.html
        assertTrue(rendered!!.contains("<h1>Title</h1>"))
        assertTrue(rendered.contains("<strong>bold</strong>"))
        assertTrue(rendered.contains("link"))
        assertFalse(rendered.contains("href="))
        assertEquals("diagram", decoded.images.single().alt)
        assertEquals("https://example.com/a.png", decoded.images.single().destination)
        assertFalse(rendered.contains("<img"))
        assertTrue(rendered.contains("&lt;script&gt;alert(1)&lt;/script&gt;"))
        val annotated = pluginMarkdownAnnotatedString(decoded, emptyMap())
        assertTrue(annotated.text.text.contains("link"))
        assertTrue(annotated.text.text.contains("[图片: diagram]"))
        assertTrue(annotated.text.text.contains("<script>alert(1)</script>"))
        assertTrue(annotated.text.spanStyles.any {
            it.item.fontWeight == androidx.compose.ui.text.font.FontWeight.Bold
        })
        assertFalse(PluginResourceMarkdownDecoder.supports("text/html", "page.html"))
        assertTrue(PluginResourceMarkdownDecoder.supports("text/markdown; charset=utf-8", "page.bin"))
        assertTrue(PluginResourceMarkdownDecoder.supports("application/octet-stream", "page.markdown"))
    }

    @Test fun markdownAttachmentResolverAcceptsOnlyCanonicalSameDirectorySiblings() {
        assertEquals("docs/diagram.png", PluginResourceMarkdownDecoder.resolveSameDirectoryImage("docs/readme.md", "diagram.png"))
        assertEquals("diagram.png", PluginResourceMarkdownDecoder.resolveSameDirectoryImage("readme.md", "diagram.png"))
        assertNull(PluginResourceMarkdownDecoder.resolveSameDirectoryImage("docs/readme.md", "../outside.png"))
        assertNull(PluginResourceMarkdownDecoder.resolveSameDirectoryImage("docs/readme.md", "sub/diagram.png"))
        assertNull(PluginResourceMarkdownDecoder.resolveSameDirectoryImage("docs/readme.md", "https://example.com/diagram.png"))
        assertNull(PluginResourceMarkdownDecoder.resolveSameDirectoryImage("docs/readme.md", "//example.com/diagram.png"))
        assertNull(PluginResourceMarkdownDecoder.resolveSameDirectoryImage("docs/readme.md", "%2e%2e/outside.png"))
    }

    @Test fun markdownRejectsMalformedUtf8AndOversizePayloads() {
        assertNull(PluginResourceMarkdownDecoder.decode(byteArrayOf(0xc3.toByte(), 0x28)))
        assertNull(PluginResourceMarkdownDecoder.decode(ByteArray(262145) { 'x'.code.toByte() }))
    }

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
