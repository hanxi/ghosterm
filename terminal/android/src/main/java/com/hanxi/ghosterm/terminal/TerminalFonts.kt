package com.hanxi.ghosterm.terminal

import android.content.Context
import android.graphics.Typeface
import android.graphics.fonts.Font
import android.graphics.fonts.FontFamily
import java.util.concurrent.ConcurrentHashMap

/** Selected text face → Nerd symbols → Android monospace/CJK/emoji fallback. */
object TerminalFonts {
    private val cache=ConcurrentHashMap<String,Typeface>()
    private val preparation=Any()
    private val order=java.util.ArrayDeque<String>()
    /** Call from an IO owner before publishing a new font ID to views. */
    fun prepare(context:Context,id:String):Typeface=synchronized(preparation) {
        // Even a warm cache must not hide a missing or altered imported file.
        val file=if(id.startsWith("custom:")) TerminalFontStore(context).verifiedFile(id) else null
        cache[id]?.let { order.remove(id);order.addLast(id);return@synchronized it }
        val symbols=FontFamily.Builder(Font.Builder(context.resources,R.font.nerd_symbols_mono).build()).build()
        val builder=if(id=="system") Typeface.CustomFallbackBuilder(symbols) else {
            val font=when(id) {
                "jetbrains_mono" -> Font.Builder(context.resources,R.font.jetbrains_mono).build()
                "source_code_pro" -> Font.Builder(context.resources,R.font.source_code_pro).build()
                else -> Font.Builder(checkNotNull(file) { "不支持的字体编号" }).build()
            }
            Typeface.CustomFallbackBuilder(FontFamily.Builder(font).build()).addCustomFallback(symbols)
        }
        builder.setSystemFallback("monospace").build().also {
            cache[id]=it;order.remove(id);order.addLast(id)
            while(order.size>24) cache.remove(order.removeFirst())
        }
    }
    fun typeface(context:Context,id:String="system"):Typeface {
        cache[id]?.let { return it }
        if(id=="system") return prepare(context,id)
        error("字体尚未载入，请重新选择字体")
    }
}
