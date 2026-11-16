package com.hanxi.ghosterm

import android.graphics.Bitmap
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionResourceStore.Content
import com.hanxi.ghosterm.bridge.ExtensionResourceStore.Descriptor
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.ByteArrayOutputStream
import java.net.URI
import java.security.MessageDigest

internal fun htmlFixture(path: String, mime: String, bytes: ByteArray, id: String = path): Content =
    Content(Descriptor(MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) },
        mime, path, bytes.size.toLong(), resourceId = id), bytes)
internal fun htmlFixturePNG(width: Int = 8, height: Int = 8): ByteArray {
    val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888).apply { eraseColor(android.graphics.Color.RED) }
    return try { ByteArrayOutputStream().use { out -> assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)); out.toByteArray() } }
    finally { bitmap.recycle() }
}

@RunWith(AndroidJUnit4::class)
class PluginResourceHTMLTest {
    private fun doc(source: ByteArray = "<h1>HTML</h1>".toByteArray()) = htmlFixture("docs/读我.html", "text/html; charset=utf-8", source)
    @Test fun htmlRequiresConfirmedExactBytesStrictUtf8AndCanonicalNames() {
        val d = doc(); val data = listOf(d)
        assertNotNull(PluginResourceHTMLDocument.capture(d, data))
        assertTrue(PluginResourceHTMLDocument.supports(" TEXT/HTML ; charset=utf-8"))
        assertFalse(PluginResourceHTMLDocument.supports("application/xhtml+xml"))
        for (bad in listOf(
            d.copy(descriptor = d.descriptor.copy(sizeKnown = false)),
            d.copy(descriptor = d.descriptor.copy(size = d.bytes.size + 1L)),
            d.copy(descriptor = d.descriptor.copy(sha256 = "0".repeat(64))),
            d.copy(descriptor = d.descriptor.copy(displayName = "docs/../bad.html")),
            d.copy(descriptor = d.descriptor.copy(displayName = "/docs/bad.html")),
            doc(byteArrayOf(0xc3.toByte(), 0x28)), doc(ByteArray(262145) { 65 })
        )) assertTrue("invalid document ${bad.descriptor}", PluginResourceHTMLDocument.capture(bad, listOf(bad)) == null)
        assertNull(PluginResourceHTMLDocument.capture(d, listOf(d, d)))
        assertNull(PluginResourceHTMLDocument.capture(d, emptyList()))
        assertFalse(PluginResourceHTMLDocument.canonical("bad\uD800.html"))
    }

    @Test fun documentHasARestrictiveFirstPolicyAndLiteralUnicodeSource() {
        val d = doc("<meta http-equiv='Content-Security-Policy' content=\"script-src *\"><h1>中文 😀</h1>".toByteArray())
        val snapshot = PluginResourceHTMLDocument.capture(d, listOf(d))!!
        val html = PluginResourceHTMLDocument.page(snapshot, "https://example.invalid/native.css")
        for (rule in listOf("default-src 'none'", "script-src 'none'", "style-src 'self'", "img-src 'self'",
            "connect-src 'none'", "frame-src 'none'", "form-action 'none'", "base-uri 'none'")) assertTrue(rule, html.contains(rule))
        assertTrue(html.indexOf("default-src 'none'") < html.indexOf("script-src *"))
        assertTrue(html.contains("<h1>中文 😀</h1>"))
    }

    @Test fun onlyFollowingExplicitBundleAssetsAreCapturedAndOriginalArraysCannotChangeThem() {
        val d = doc(); val css = htmlFixture("docs/theme.css", "text/css", "body{color:blue}".toByteArray())
        val old = htmlFixture("docs/old.css", "text/css", "old".toByteArray())
        val other = htmlFixture("outside.css", "text/css", "outside".toByteArray())
        val image = htmlFixture("docs/marker.png", "image/png", htmlFixturePNG())
        val next = htmlFixture("docs/next.md", "text/markdown", "next".toByteArray())
        val later = htmlFixture("docs/later.css", "text/css", "later".toByteArray())
        val snapshot = PluginResourceHTMLDocument.capture(d, listOf(old, d, css, image, other, next, later))!!
        assertEquals(setOf("docs/theme.css", "docs/marker.png"), snapshot.assets.keys)
        css.bytes.fill(0); d.bytes.fill(0)
        assertEquals("body{color:blue}", snapshot.assets["docs/theme.css"]!!.bytes.toString(Charsets.UTF_8))
        assertEquals("<h1>HTML</h1>", snapshot.source)
        assertEquals("image/png", snapshot.assets["docs/marker.png"]!!.mime)
        assertTrue(snapshot.omittedAttachments)
    }

    @Test fun duplicatePathsBadMetadataAndExecutableImageTypesStayUnavailable() {
        val d = doc(); val css = htmlFixture("docs/duplicate.css", "text/css", "one".toByteArray())
        val again = htmlFixture("docs/duplicate.css", "text/plain", "two".toByteArray(), "other")
        val svg = htmlFixture("docs/vector.svg", "image/svg+xml", "<svg/>".toByteArray())
        val big = htmlFixture("docs/wide.png", "image/png", htmlFixturePNG(2049, 1))
        val bad = htmlFixture("docs/bad.css", "text/css", "ok".toByteArray()).let { it.copy(descriptor = it.descriptor.copy(sha256 = "0".repeat(64))) }
        val snapshot = PluginResourceHTMLDocument.capture(d, listOf(d, css, again, svg, big, bad))!!
        assertTrue(snapshot.assets.isEmpty()); assertTrue(snapshot.omittedAttachments)
    }

    @Test fun attachmentCountCssBytesAndTotalDecodedPixelsHaveSeparateLimits() {
        val d = doc()
        val css = (0..6).map { htmlFixture("docs/$it.css", "text/css", "x".toByteArray()) }
        val snapshot = PluginResourceHTMLDocument.capture(d, listOf(d) + css)!!
        assertEquals(6, snapshot.assets.size); assertTrue(snapshot.omittedAttachments)
        val large = htmlFixture("docs/large.css", "text/css", ByteArray(262145) { 65 })
        assertTrue(PluginResourceHTMLDocument.capture(d, listOf(d, large))!!.assets.isEmpty())
        val png = htmlFixturePNG(2048, 2048)
        val first = htmlFixture("docs/first.png", "image/png", png)
        val second = htmlFixture("docs/second.png", "image/png", png)
        val limited = PluginResourceHTMLDocument.capture(d, listOf(d, first, second))!!
        assertEquals(setOf("docs/first.png"), limited.assets.keys); assertTrue(limited.omittedAttachments)
    }

    @Test fun effectiveAssetsAndDocumentPathChangeTheViewFingerprint() {
        val d = doc(); val css = htmlFixture("docs/theme.css", "text/css", "a".toByteArray())
        fun capture(document: Content, asset: Content) = PluginResourceHTMLDocument.capture(document, listOf(document, asset))!!
        val before = capture(d, css)
        assertEquals(before.fingerprint, capture(d, css.copy(bytes = css.bytes.copyOf())).fingerprint)
        assertNotEquals(before.fingerprint, capture(d, css.copy(descriptor = css.descriptor.copy(mime = "text/plain"))).fingerprint)
        assertNotEquals(before.fingerprint, capture(d, htmlFixture("docs/theme.css", "text/css", "b".toByteArray())).fingerprint)
        assertNotEquals(before.fingerprint, capture(d.copy(descriptor = d.descriptor.copy(displayName = "docs/other.html")), css).fingerprint)
    }

    @Test fun unicodeSpacesAndLiteralPercentNamesResolveWithoutPathTraversal() {
        val d = htmlFixture("docs/页面 %.html", "text/html", "<p>中文</p>".toByteArray())
        val css = htmlFixture("docs/图 标%.css", "text/css", "body{color:blue}".toByteArray())
        val owner = PluginResourceHTMLCoordinator(PluginResourceHTMLDocument.capture(d, listOf(d, css))!!)
        val base = URI(owner.documentURL)
        val address = base.resolve(URI(null, null, "图 标%.css", null)).toASCIIString()
        assertEquals(200, owner.intercept(address, "GET", false).statusCode)
        assertEquals(403, owner.intercept("https://${base.host}/docs/%2e%2e/图.css", "GET", false).statusCode)
        assertEquals(403, owner.intercept("https://${base.host}/docs/%FF.css", "GET", false).statusCode)
        owner.close()
        assertEquals(403, owner.intercept(address, "GET", false).statusCode)
    }

    @Test fun syntheticOriginServesOnlyMappedGetSubresourcesAndCloseRevokesIt() {
        val d = doc(); val css = htmlFixture("docs/theme.css", "text/css", "body{color:blue}".toByteArray())
        val owner = PluginResourceHTMLCoordinator(PluginResourceHTMLDocument.capture(d, listOf(d, css))!!)
        owner.intercept(owner.documentURL, "GET", true).let {
            assertEquals(200, it.statusCode)
            assertEquals(PluginResourceHTMLDocument.CSP, it.responseHeaders["Content-Security-Policy"])
            assertTrue(it.data.use { stream -> stream.readBytes().toString(Charsets.UTF_8) }.contains("<h1>HTML</h1>"))
        }
        val base = URI(owner.documentURL)
        val allowed = base.resolve("theme.css").toString()
        owner.intercept(allowed, "GET", false).let { assertEquals(200, it.statusCode); assertEquals("body{color:blue}", it.data.use { s -> s.readBytes().toString(Charsets.UTF_8) }) }
        for (url in listOf("https://elsewhere.invalid/docs/theme.css", "http://${base.host}/docs/theme.css",
            "https://user@${base.host}/docs/theme.css", "https://${base.host}:443/docs/theme.css",
            "$allowed?query=1", "$allowed#fragment", base.resolve("missing.css").toString(),
            "file:///etc/passwd", "content://provider/private", "data:text/css,body{}")) {
            assertEquals(url, 403, owner.intercept(url, "GET", false).statusCode)
        }
        assertEquals(403, owner.intercept(allowed, "POST", false).statusCode)
        assertEquals(403, owner.intercept(allowed, "GET", true).statusCode)
        owner.close(); owner.close(); owner.load()
        assertTrue(owner.closed); assertEquals(403, owner.intercept(allowed, "GET", false).statusCode)
    }
}
