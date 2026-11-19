package com.hanxi.ghosterm

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import com.hanxi.ghosterm.bridge.ExtensionResourceStore.Content
import java.io.ByteArrayOutputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest

/** Captures only this document's following, explicitly received attachments.
 * No disk, URL fetch, JavaScript, original workspace path or active Session. */
internal object PluginResourceHTMLDocument {
    const val MAX_DOCUMENT_BYTES = 262144
    const val MAX_ASSET_BYTES = 1048576
    const val MAX_ATTACHMENTS = 6
    const val MAX_IMAGE_PIXELS = 4194304L
    const val CSP = "default-src 'none'; script-src 'none'; style-src 'self'; img-src 'self'; " +
        "font-src 'none'; media-src 'none'; connect-src 'none'; object-src 'none'; " +
        "frame-src 'none'; child-src 'none'; form-action 'none'; base-uri 'none'"

    data class Asset(val mime: String, val bytes: ByteArray)
    data class Snapshot(val path: String, val source: String, val fingerprint: String,
        val assets: Map<String, Asset>, val omittedAttachments: Boolean)

    fun supports(mime: String) = normalized(mime) == "text/html"
    private fun normalized(mime: String) = mime.substringBefore(';').trim().lowercase()
    fun canonical(path: String): Boolean = path.isNotEmpty() && path.toByteArray().size <= 1024 &&
        !path.startsWith('/') && !path.contains('\\') && !path.contains('\u0000') && Charsets.UTF_8.newEncoder().canEncode(path) &&
        path.split('/').all { it.isNotEmpty() && it != "." && it != ".." }

    private fun utf8(bytes: ByteArray): String? = try {
        Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
    } catch (_: java.nio.charset.CharacterCodingException) { null }

    /** Decode escaped bytes exactly once. URI.path replaces malformed UTF-8;
     * that would alias an invalid URL to a valid U+FFFD snapshot filename. */
    fun requestPath(raw: String?): String? {
        if (raw == null || !raw.startsWith('/') || raw.length > 3073 ||
            !Charsets.UTF_8.newEncoder().canEncode(raw)) return null
        val input = raw.toByteArray(Charsets.UTF_8)
        val output = ByteArray(input.size)
        var at = 0; var count = 0
        while (at < input.size) {
            if (input[at] == '%'.code.toByte()) {
                if (at + 2 >= input.size) return null
                val high = (input[at + 1].toInt() and 255).toChar().digitToIntOrNull(16) ?: return null
                val low = (input[at + 2].toInt() and 255).toChar().digitToIntOrNull(16) ?: return null
                output[count++] = (high * 16 + low).toByte(); at += 3
            } else output[count++] = input[at++]
        }
        return utf8(output.copyOf(count))?.removePrefix("/")?.takeIf(::canonical)
    }

    private fun sha(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes)
        .joinToString("") { "%02x".format(it.toInt() and 255) }

    private fun fixed(resource: Content, limit: Int): ByteArray? {
        val d = resource.descriptor
        if (!d.sizeKnown || d.size != resource.bytes.size.toLong() || resource.bytes.size > limit) return null
        val bytes = resource.bytes.copyOf()
        return bytes.takeIf { sha(it) == d.sha256 }
    }

    /** BitmapFactory's first frame is re-encoded as bounded static PNG. WebView
     * never receives a compressed animation or a container with excessive pixels. */
    private fun image(bytes: ByteArray, remainingPixels: Long): Pair<Asset, Long>? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        val pixels = bounds.outWidth.toLong() * bounds.outHeight
        if (bounds.outWidth !in 1..2048 || bounds.outHeight !in 1..2048 ||
            pixels > remainingPixels || pixels > MAX_IMAGE_PIXELS) return null
        val bitmap = decodePluginResourceBitmap(bytes) ?: return null
        try {
            val output = ByteArrayOutputStream()
            val bounded = object : OutputStream() {
                override fun write(value: Int) {
                    if (output.size() == MAX_ASSET_BYTES) throw Limit()
                    output.write(value)
                }
                override fun write(bytes: ByteArray, offset: Int, count: Int) {
                    if (count > MAX_ASSET_BYTES - output.size()) throw Limit()
                    output.write(bytes, offset, count)
                }
            }
            return try {
                if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, bounded)) null
                else Asset("image/png", output.toByteArray()) to pixels
            } catch (_: Limit) { null }
        } finally { bitmap.recycle() }
    }
    private class Limit : java.io.IOException()

    fun capture(document: Content, resources: List<Content>, checkCancellation: () -> Unit = {}): Snapshot? {
        checkCancellation()
        val d = document.descriptor
        if (!supports(d.mime) || !canonical(d.displayName)) return null
        val sourceBytes = fixed(document, MAX_DOCUMENT_BYTES) ?: return null
        val source = utf8(sourceBytes) ?: return null
        val positions = resources.indices.filter { resources[it].descriptor.resourceId == d.resourceId }
        if (d.resourceId.isEmpty() || positions.size != 1) return null
        val directory = d.displayName.substringBeforeLast('/', "").let { if (it.isEmpty()) "" else "$it/" }
        val candidates = resources.drop(positions.single() + 1).takeWhile {
            !supports(it.descriptor.mime) && !PluginResourceMarkdownDecoder.supports(it.descriptor.mime, it.descriptor.displayName)
        }
        val assets = linkedMapOf<String, Asset>()
        val seen = mutableSetOf<String>()
        var omitted = candidates.size > MAX_ATTACHMENTS
        var pixels = 0L
        for (resource in candidates.take(MAX_ATTACHMENTS)) {
            checkCancellation()
            val descriptor = resource.descriptor
            val path = descriptor.displayName
            if (!canonical(path) || !path.startsWith(directory)) { omitted = true; continue }
            if (!seen.add(path)) { assets.remove(path); omitted = true; continue }
            val bytes = fixed(resource, MAX_ASSET_BYTES)
            if (bytes == null) { omitted = true; continue }
            val mime = normalized(descriptor.mime)
            val asset = when (mime) {
                "text/css" -> if (bytes.size <= MAX_DOCUMENT_BYTES && utf8(bytes) != null) Asset(mime, bytes) else null
                "image/png", "image/jpeg", "image/gif", "image/webp" -> image(bytes, MAX_IMAGE_PIXELS - pixels)?.let {
                    pixels += it.second; it.first
                }
                else -> null
            }
            if (asset == null) omitted = true else assets[path] = asset
        }
        checkCancellation()
        val identity = mutableListOf(d.resourceId, d.generation.toString(), d.displayName, d.sha256, omitted.toString())
        for ((path, asset) in assets) {
            checkCancellation()
            identity += listOf(path, asset.mime, sha(asset.bytes))
        }
        val fingerprint = MessageDigest.getInstance("SHA-256")
        for (field in identity) {
            val bytes = field.toByteArray()
            fingerprint.update(ByteBuffer.allocate(4).putInt(bytes.size).array()); fingerprint.update(bytes)
        }
        return Snapshot(d.displayName, source, fingerprint.digest().joinToString("") { "%02x".format(it.toInt() and 255) },
            assets.toMap(), omitted)
    }

    fun page(snapshot: Snapshot, stylesheet: String): String =
        "<!doctype html><html><head><meta charset=\"utf-8\">" +
            "<meta http-equiv=\"Content-Security-Policy\" content=\"$CSP\">" +
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">" +
            "<link rel=\"stylesheet\" href=\"$stylesheet\"></head><body>" + snapshot.source + "</body></html>"
}
