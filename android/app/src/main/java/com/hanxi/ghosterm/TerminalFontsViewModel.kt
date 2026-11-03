package com.hanxi.ghosterm

import android.app.Application
import android.graphics.Typeface
import android.net.Uri
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.data.FontDocumentReader
import com.hanxi.ghosterm.domain.TerminalFont
import com.hanxi.ghosterm.terminal.TerminalFontStore
import com.hanxi.ghosterm.terminal.TerminalFonts
import kotlinx.coroutines.*
import java.util.UUID

data class TerminalFontChoice(val value:TerminalFont,val label:String,val typeface:Typeface?,val problem:String?=null)

class TerminalFontsViewModel(application:Application):AndroidViewModel(application) {
    private val store=TerminalFontStore(application)
    private val client=UUID.randomUUID()
    var choices by mutableStateOf<List<TerminalFontChoice>>(emptyList());private set
    var busy by mutableStateOf(false);private set
    var pickerPending by mutableStateOf(false);private set
    var error by mutableStateOf<String?>(null);private set
    var notice by mutableStateOf<String?>(null);private set
    private var importJob:Job?=null
    private data class Inventory(val choices:List<TerminalFontChoice>,val problem:String?)
    private fun apply(inventory:Inventory) { choices=inventory.choices;inventory.problem?.let { error=it } }
    private fun load():Inventory {
        var problem:String?=null
        val imported=try { store.list().map { Triple(checkNotNull(TerminalFont.fromId(it.id)),it.label,it.problem) } }
            catch(failure:Exception) { problem="无法读取导入字体：${failure.message ?: "未知原因"}";emptyList() }
        val items=TerminalFont.entries.zip(listOf("系统等宽字体","JetBrains Mono","Source Code Pro")).map { (font,label) -> Triple(font,label,null as String?) }+imported
        return Inventory(items.map { (font,label,issue) ->
            if(issue!=null) TerminalFontChoice(font,label,null,issue) else try {
                TerminalFontChoice(font,label,TerminalFonts.prepare(getApplication(),font.id))
            } catch(failure:Exception) { TerminalFontChoice(font,label,null,failure.message ?: "字体无法载入") }
        },problem)
    }
    fun refresh() {
        if(busy || pickerPending) return
        busy=true;error=null
        viewModelScope.launch {
            try { apply(withContext(Dispatchers.IO) { load() }) }
            catch(cancelled:CancellationException) { throw cancelled }
            catch(failure:Exception) { error="无法读取字体：${failure.message ?: "未知原因"}" }
            finally { busy=false }
        }
    }
    fun beginImport():Boolean {
        if(busy || pickerPending) return false
        pickerPending=true;error=null;notice=null;return true
    }
    fun importLaunchFailed() { pickerPending=false;error="无法打开系统文件选择器" }
    fun cancelImport() { pickerPending=false;importJob?.cancel() }
    fun completeImport(uri:Uri?) {
        val expected=pickerPending;pickerPending=false
        if(!expected || uri==null || busy) return
        busy=true;error=null;notice=null
        importJob=viewModelScope.launch {
            try {
                val document=FontDocumentReader.read(getApplication<Application>().contentResolver,uri)
                val entry=withContext(Dispatchers.IO) { val context=currentCoroutineContext();store.import(document.name,document.bytes) { context.ensureActive() } }
                apply(withContext(Dispatchers.IO) { load() });notice="已导入 ${entry.label}，点选后应用"
            } catch(timeout:TimeoutCancellationException) { error="读取字体超时，请重试"
            } catch(cancelled:CancellationException) { throw cancelled }
            catch(failure:Exception) { error="导入字体未完成：${failure.message ?: "未知原因"}。可重新载入检查已有字体。" }
            finally { busy=false }
        }
    }
    fun remove(font:TerminalFont,settings:AppSettings) {
        if(busy || pickerPending) return
        busy=true;error=null;notice=null
        viewModelScope.launch {
            try {
                val failure=settings.removeFont(client,font.id)
                if(failure!=null) error=failure else notice="字体已移除"
                apply(withContext(Dispatchers.IO) { load() })
            } catch(cancelled:CancellationException) { throw cancelled }
            catch(failure:Exception) { error=failure.message ?: "无法重新载入字体" }
            finally { busy=false }
        }
    }
}
