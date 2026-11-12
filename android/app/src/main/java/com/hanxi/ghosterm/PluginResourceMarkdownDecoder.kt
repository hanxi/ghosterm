package com.hanxi.ghosterm

import org.commonmark.node.Code
import org.commonmark.node.HardLineBreak
import org.commonmark.node.Image
import org.commonmark.node.Link
import org.commonmark.node.Node
import org.commonmark.node.SoftLineBreak
import org.commonmark.node.Text
import org.commonmark.parser.Parser
import org.commonmark.renderer.html.HtmlRenderer
import java.net.URI

internal object PluginResourceMarkdownDecoder {
    private const val MAX_SOURCE_BYTES = 262144
    private const val MAX_RENDERED_HTML_CHARS = 1048576
    data class ImageReference(val marker: String, val alt: String, val destination: String)
    data class Decoded(val html: String, val images: List<ImageReference>)

    fun supports(mime: String, displayName: String): Boolean =
        mime.substringBefore(';').trim().lowercase() in setOf("text/markdown", "text/x-markdown") ||
            displayName.substringAfterLast('.', "").lowercase() in setOf("md", "markdown")

    fun decode(bytes: ByteArray): String? {
        return decodeDocument(bytes)?.html
    }

    fun decodeDocument(bytes: ByteArray): Decoded? {
        if (bytes.isEmpty() || bytes.size > MAX_SOURCE_BYTES) return null
        val source = try {
            Charsets.UTF_8.newDecoder()
                .onMalformedInput(java.nio.charset.CodingErrorAction.REPORT)
                .onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPORT)
                .decode(java.nio.ByteBuffer.wrap(bytes)).toString()
        } catch (_: java.nio.charset.CharacterCodingException) {
            return null
        }
        val tokenPrefix = generateSequence(0) { it + 1 }
            .map { "GHOSTERM_PREVIEW_IMAGE_${it}_" }
            .first { !source.contains(it) }
        val images = mutableListOf<ImageReference>()
        val document = Parser.builder().build().parse(source)
        sanitize(document, tokenPrefix, images)
        val html = HtmlRenderer.builder().escapeHtml(true).sanitizeUrls(true).build().render(document)
            .takeIf { it.length <= MAX_RENDERED_HTML_CHARS } ?: return null
        return Decoded(html, images.toList())
    }

    fun resolveSameDirectoryImage(documentPath: String, destination: String): String? {
        if (!isCanonicalWorkspacePath(documentPath)) return null
        val uri = try { URI(destination) } catch (_: Exception) { return null }
        if (uri.isAbsolute || uri.rawAuthority != null || uri.rawQuery != null || uri.rawFragment != null) return null
        val path = uri.path ?: return null
        if (path.isEmpty() || path == "." || path == ".." || path.contains('/') || path.contains('\\') ||
            path.any { it == '\u0000' }) return null
        val directory = documentPath.substringBeforeLast('/', "")
        val candidate = if (directory.isEmpty()) path else "$directory/$path"
        return candidate.takeIf(::isCanonicalWorkspacePath)
    }

    fun isCanonicalWorkspacePath(path: String): Boolean {
        if (path.isEmpty() || path.startsWith('/') || path.startsWith('\\') || path.contains('\\') || path.length > 1024) return false
        val parts = path.split('/')
        return parts.all { part -> part.isNotEmpty() && part != "." && part != ".." && part.none { it == '\u0000' } }
    }

    private fun sanitize(parent: Node, tokenPrefix: String, images: MutableList<ImageReference>) {
        var child = parent.firstChild
        while (child != null) {
            val next = child.next
            sanitize(child, tokenPrefix, images)
            when (child) {
                is Link -> {
                    var nested = child.firstChild
                    while (nested != null) {
                        val after = nested.next
                        child.insertBefore(nested)
                        nested = after
                    }
                    child.unlink()
                }
                is Image -> {
                    val alt = buildString {
                        fun appendText(node: Node?) {
                            var item = node
                            while (item != null) {
                                when (item) {
                                    is Text -> append(item.literal)
                                    is Code -> append(item.literal)
                                    is SoftLineBreak, is HardLineBreak -> append(' ')
                                    else -> appendText(item.firstChild)
                                }
                                item = item.next
                            }
                        }
                        appendText(child.firstChild)
                    }.trim()
                    val marker = "$tokenPrefix${images.size}END"
                    images += ImageReference(marker, alt, child.destination)
                    Text(marker).also { child.insertBefore(it) }
                    child.unlink()
                }
            }
            child = next
        }
    }
}
