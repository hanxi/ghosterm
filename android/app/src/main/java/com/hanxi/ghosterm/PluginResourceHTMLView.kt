package com.hanxi.ghosterm

import android.content.Context
import android.graphics.Color
import android.net.Uri
import android.view.View
import android.view.ViewGroup
import android.webkit.*
import android.widget.FrameLayout
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.hanxi.ghosterm.bridge.ExtensionResourceStore.Content
import java.io.ByteArrayInputStream
import java.net.URI
import java.util.UUID

/** One immutable preview and one fresh synthetic origin. Requests run off the
 * UI thread; close revokes future reads without changing global WebView stores. */
internal class PluginResourceHTMLCoordinator(snapshot: PluginResourceHTMLDocument.Snapshot,
    private var issue: ((String) -> Unit)? = null) {
    private val lock = Any()
    private var content: PluginResourceHTMLDocument.Snapshot? = snapshot
    private var web: WebView? = null
    private var loaded = false
    private var visible = false
    private var deadline: Runnable? = null
    private val host = "p${UUID.randomUUID().toString().replace("-", "")}.ghosterm-preview.invalid"
    val documentURL = URI("https", null, host, -1, "/${snapshot.path}", null, null).toASCIIString()
    private val stylesheetURL = "https://$host/" + generateSequence(0) { it + 1 }
        .map { ".ghosterm-native-$it/style.css" }.first { it != snapshot.path && it !in snapshot.assets }
    private var stylesheet = byteArrayOf()
    val closed get() = synchronized(lock) { content == null }

    private fun response(mime: String, data: ByteArray, status: Int): WebResourceResponse =
        WebResourceResponse(mime, if (mime.startsWith("text/")) "utf-8" else null,
            status, if (status == 200) "OK" else "Blocked", mapOf(
                "Cache-Control" to "no-store, no-cache, must-revalidate",
                "Content-Length" to data.size.toString(), "X-Content-Type-Options" to "nosniff",
                "Referrer-Policy" to "no-referrer", "X-Frame-Options" to "DENY",
                "Content-Security-Policy" to PluginResourceHTMLDocument.CSP), ByteArrayInputStream(data))

    fun intercept(url: String, method: String, mainFrame: Boolean): WebResourceResponse = synchronized(lock) {
        val snapshot = content
        val uri = try { URI(url) } catch (_: Exception) { null }
        val allowed = snapshot != null && method == "GET" && uri != null &&
            uri.scheme == "https" && uri.rawAuthority == host && uri.rawQuery == null && uri.rawFragment == null
        if (!allowed) return@synchronized response("text/plain", byteArrayOf(), 403)
        if (mainFrame) return@synchronized if (url == documentURL)
            response("text/html", PluginResourceHTMLDocument.page(snapshot!!, stylesheetURL).toByteArray(), 200)
            else response("text/plain", byteArrayOf(), 403)
        if (url == stylesheetURL) return@synchronized response("text/css", stylesheet, 200)
        val path = uri!!.path?.removePrefix("/")
        val asset = path?.takeIf(PluginResourceHTMLDocument::canonical)?.let { snapshot!!.assets[it] }
        if (asset == null) response("text/plain", byteArrayOf(), 403) else response(asset.mime, asset.bytes, 200)
    }

    val client = object : WebViewClient() {
        override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest) =
            intercept(request.url.toString(), request.method, request.isForMainFrame)
        override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest) = true
        @Suppress("DEPRECATION")
        override fun shouldOverrideUrlLoading(view: WebView, url: String) = true
        override fun onPageCommitVisible(view: WebView, url: String) {
            if (!closed && url == documentURL) {
                visible = true
                deadline?.let(view::removeCallbacks); deadline = null
            }
        }
        override fun onPageFinished(view: WebView, url: String) { onPageCommitVisible(view, url) }
        override fun onReceivedError(view: WebView, request: WebResourceRequest, error: WebResourceError) {
            if (request.isForMainFrame) fail("HTML 预览加载失败，可查看源码。")
        }
        override fun onRenderProcessGone(view: WebView, detail: RenderProcessGoneDetail): Boolean {
            fail("HTML 预览进程已退出，可查看源码。")
            return true
        }
    }

    @Suppress("DEPRECATION")
    fun create(context: Context, foreground: Int, background: Int, textZoom: Int): WebView {
        check(!closed && web == null)
        return WebView(context).also { view ->
            web = view
            view.settings.apply {
                javaScriptEnabled = false
                javaScriptCanOpenWindowsAutomatically = false
                setSupportMultipleWindows(false)
                allowFileAccess = false
                allowContentAccess = false
                allowFileAccessFromFileURLs = false
                allowUniversalAccessFromFileURLs = false
                blockNetworkLoads = true
                domStorageEnabled = false
                databaseEnabled = false
                saveFormData = false
                cacheMode = WebSettings.LOAD_NO_CACHE
                mixedContentMode = WebSettings.MIXED_CONTENT_NEVER_ALLOW
                mediaPlaybackRequiresUserGesture = true
                setGeolocationEnabled(false)
                // Even intercepted URLs can trigger Safe Browsing lookups.
                // This view serves only fixed local snapshots and has no network fallback.
                safeBrowsingEnabled = false
                this.textZoom = textZoom
            }
            CookieManager.getInstance().setAcceptThirdPartyCookies(view, false)
            view.isSaveEnabled = false
            view.importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS
            view.isFocusable = false; view.isFocusableInTouchMode = false
            view.isLongClickable = false; view.setOnLongClickListener { true }
            view.setBackgroundColor(background)
            view.webViewClient = client
            view.webChromeClient = object : WebChromeClient() {
                override fun onConsoleMessage(message: ConsoleMessage) = true
                override fun onPermissionRequest(request: PermissionRequest) { request.deny() }
                override fun onShowFileChooser(view: WebView, callback: android.webkit.ValueCallback<Array<Uri>>,
                    parameters: FileChooserParams): Boolean { callback.onReceiveValue(null); return true }
                override fun onCreateWindow(view: WebView, dialog: Boolean, gesture: Boolean, message: android.os.Message) = false
            }
            view.setDownloadListener { _, _, _, _, _ -> }
            val fg = String.format("#%06x", foreground and 0xffffff)
            val bg = String.format("#%06x", background and 0xffffff)
            synchronized(lock) {
                stylesheet = "html{color:$fg;background:$bg}body{margin:12px;overflow-wrap:anywhere}img{max-width:100%;height:auto}".toByteArray()
            }
        }
    }

    fun load() {
        if (loaded || closed) return
        synchronized(lock) { content } ?: return
        val target = web ?: return
        loaded = true
        deadline = Runnable { if (!visible) fail("HTML 预览超时，可查看源码。") }.also { target.postDelayed(it, 10000) }
        target.loadUrl(documentURL)
    }

    private fun fail(message: String) {
        if (!closed) { issue?.invoke(message); close() }
    }

    fun close() {
        synchronized(lock) {
            if (content == null) return
            content = null; stylesheet = byteArrayOf()
        }
        issue = null
        web?.let { view ->
            deadline?.let(view::removeCallbacks); deadline = null
            view.stopLoading()
            view.webViewClient = object : WebViewClient() {
                override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest) = response("text/plain", byteArrayOf(), 403)
                override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest) = true
            }
            view.webChromeClient = null; view.setDownloadListener(null)
            (view.parent as? ViewGroup)?.removeView(view)
            view.destroy()
        }
        web = null
    }
}

@Composable internal fun PluginHTMLBody(document: Content, resources: List<Content>) {
    var source by remember(document.descriptor.resourceId, document.descriptor.sha256) { mutableStateOf(false) }
    Column {
        TextButton(onClick = { source = !source }, modifier = Modifier.testTag("plugin-resource-html-source-toggle")) {
            Text(if (source) "返回 HTML 预览" else "查看源码")
        }
        if (source) {
            Text("源码预览（最多 8192 字符）", Modifier.padding(horizontal = 12.dp), style = MaterialTheme.typography.labelSmall)
            decodePluginResourceText(document.bytes)?.let { text -> SelectionContainer {
                Text(text, Modifier.fillMaxWidth().heightIn(max = 280.dp).verticalScroll(rememberScrollState()).padding(12.dp),
                    style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace))
            } } ?: Text("源码不是有效 UTF-8 或超出显示预算", Modifier.padding(12.dp))
        } else {
            val descriptors = resources.map { it.descriptor }
            var ready by remember(document.descriptor, descriptors) { mutableStateOf(false) }
            val snapshot by produceState<PluginResourceHTMLDocument.Snapshot?>(null, document.descriptor, descriptors) {
                value = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.Default) {
                    PluginResourceHTMLDocument.capture(document, resources)
                }
                ready = true
            }
            val captured = snapshot
            if (!ready) Text("正在准备 HTML 预览…", Modifier.padding(12.dp))
            else if (captured == null) Text("HTML 文档无法安全预览，可查看源码。", Modifier.padding(12.dp))
            else {
                key(captured.fingerprint) {
                    val fg = MaterialTheme.colorScheme.onSurface.toArgb()
                    val bg = MaterialTheme.colorScheme.surface.toArgb()
                    val zoom = (LocalDensity.current.fontScale * 100).toInt()
                    key(fg, bg, zoom) {
                        var failure by remember { mutableStateOf<String?>(null) }
                        val owner = remember { PluginResourceHTMLCoordinator(captured) { failure = it } }
                        DisposableEffect(owner) { onDispose { owner.close() } }
                        if (failure != null) Text(failure!!, Modifier.padding(12.dp))
                        else AndroidView(factory = { context ->
                            try { owner.create(context, fg, bg, zoom) }
                            catch (_: RuntimeException) {
                                failure = "此设备暂时无法打开 HTML 预览，可查看源码。"
                                FrameLayout(context)
                            }
                        }, modifier = Modifier.fillMaxWidth().height(280.dp).testTag("plugin-resource-html"),
                            onRelease = { owner.close() }, update = { owner.load() })
                        if (captured.omittedAttachments) Text("部分附件超出预览范围或未通过校验。",
                            Modifier.padding(horizontal = 12.dp), style = MaterialTheme.typography.labelSmall)
                    }
                }
            }
        }
        Text("仅加载显式附件；脚本、外链和表单提交不启用。", Modifier.padding(horizontal = 12.dp, vertical = 4.dp),
            style = MaterialTheme.typography.labelSmall)
    }
}
