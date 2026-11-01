package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.*
import org.json.JSONObject

data class AboutBuild(val commit: String?, val sourceState: String) {
    val statusText get() = when (sourceState) {
        "clean" -> "工作区干净"; "dirty" -> "包含未提交的修改"; else -> "工作区状态无法确认"
    }
    companion object {
        fun decode(text: String): AboutBuild {
            android.util.JsonReader(java.io.StringReader(text)).use { reader ->
                reader.isLenient = false
                val keys = mutableSetOf<String>()
                reader.beginObject()
                while (reader.hasNext()) { require(keys.add(reader.nextName())); reader.skipValue() }
                reader.endObject()
                require(reader.peek() == android.util.JsonToken.END_DOCUMENT)
            }
            val value = JSONObject(text)
            require(value.keys().asSequence().toSet() == setOf("schema", "git_commit", "source_state"))
            require(value.get("schema") is Int && value.getInt("schema") == 1)
            val commit = if (value.isNull("git_commit")) null else value.get("git_commit").let {
                require(it is String && it.matches(Regex("[0-9a-f]{40}|[0-9a-f]{64}"))); it
            }
            val state = value.get("source_state")
            require(state is String && state in setOf("clean", "dirty", "unknown"))
            require(commit != null || state == "unknown")
            return AboutBuild(commit, state)
        }
    }
}

enum class AboutLicense(val title: String, val fileName: String) {
    GHOSTERM("Ghosterm · MIT", "Ghosterm-MIT.txt"), GHOSTTY("Ghostty · MIT", "Ghostty-MIT.txt"),
    LIBSSH2("libssh2 · BSD", "libssh2-1.11.1.txt"), OPENSSL("OpenSSL · Apache 2.0", "openssl-3.5.4.txt"),
    NERD_FONTS("Nerd Fonts · 字体与图标许可", "NerdFontsSymbols.txt")
}
sealed interface AboutDocument {
    data object Loading : AboutDocument
    data class Ready(val text: String) : AboutDocument
    data object Failed : AboutDocument
}

class AboutViewModel @JvmOverloads constructor(application: Application,
    private val readAsset: (String) -> String = { name -> application.assets.open(name).bufferedReader(Charsets.UTF_8).use { it.readText() } }
) : AndroidViewModel(application) {
    var build by mutableStateOf<AboutBuild?>(null); private set
    var buildFailed by mutableStateOf(false); private set
    private var buildJob: Job? = null
    var selected by mutableStateOf<AboutLicense?>(null); private set
    var document by mutableStateOf<AboutDocument>(AboutDocument.Loading); private set
    var linkError by mutableStateOf<String?>(null); private set
    private var documentJob: Job? = null
    private var ticket = 0L
    fun enter() {
        if (build != null || buildJob?.isActive == true) return
        buildFailed = false
        buildJob = viewModelScope.launch {
            try { build = withContext(Dispatchers.IO) { AboutBuild.decode(readAsset("ghosterm-build.json")) } }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (_: Exception) { buildFailed = true }
        }
    }
    fun open(license: AboutLicense) {
        selected = license; document = AboutDocument.Loading
        val current = ++ticket
        documentJob?.cancel()
        documentJob = viewModelScope.launch {
            val result = try {
                val text = withContext(Dispatchers.IO) { readAsset("licenses/${license.fileName}") }
                require(text.isNotBlank()); AboutDocument.Ready(text)
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (_: Exception) { AboutDocument.Failed }
            if (ticket == current && selected == license) document = result
        }
    }
    fun back(): Boolean {
        if (selected == null) return false
        ticket++; documentJob?.cancel(); selected = null; document = AboutDocument.Loading
        return true
    }
    fun sourceUnavailable() { linkError = "无法打开项目网页，可复制项目地址后打开。" }
    fun openSource(context: android.content.Context) {
        linkError = null
        try { context.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(SOURCE))) }
        catch (_: android.content.ActivityNotFoundException) { sourceUnavailable() }
        catch (_: SecurityException) { sourceUnavailable() }
    }
    companion object { const val SOURCE = "https://github.com/hanxi/ghosterm" }
}
