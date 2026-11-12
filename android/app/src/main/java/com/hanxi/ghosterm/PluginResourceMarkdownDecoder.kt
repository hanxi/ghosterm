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

internal object PluginResourceMarkdownDecoder {
    private const val MAX_SOURCE_BYTES = 262144
    private const val MAX_RENDERED_HTML_CHARS = 1048576
    private val parser = Parser.builder().build()
    private val renderer = HtmlRenderer.builder().escapeHtml(true).sanitizeUrls(true).build()

    fun supports(mime: String, displayName: String): Boolean =
        mime.substringBefore(';').trim().lowercase() in setOf("text/markdown", "text/x-markdown") ||
            displayName.substringAfterLast('.', "").lowercase() in setOf("md", "markdown")

    fun decode(bytes: ByteArray): String? {
        if (bytes.isEmpty() || bytes.size > MAX_SOURCE_BYTES) return null
        val source = try {
            Charsets.UTF_8.newDecoder()
                .onMalformedInput(java.nio.charset.CodingErrorAction.REPORT)
                .onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPORT)
                .decode(java.nio.ByteBuffer.wrap(bytes)).toString()
        } catch (_: java.nio.charset.CharacterCodingException) {
            return null
        }
        val document = parser.parse(source)
        sanitize(document)
        return renderer.render(document).takeIf { it.length <= MAX_RENDERED_HTML_CHARS }
    }

    private fun sanitize(parent: Node) {
        var child = parent.firstChild
        while (child != null) {
            val next = child.next
            sanitize(child)
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
                    Text(if (alt.isEmpty()) "[图片]" else "[图片: $alt]").also { child.insertBefore(it) }
                    child.unlink()
                }
            }
            child = next
        }
    }
}
