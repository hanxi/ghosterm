package com.hanxi.ghosterm.terminal

import android.content.Context
import android.graphics.Typeface
import android.graphics.fonts.Font
import android.graphics.fonts.FontFamily

/** Symbols only: regular text, CJK and emoji retain Android's monospace fallback. */
internal object TerminalFonts {
    @Volatile private var cached: Typeface? = null
    fun typeface(context: Context): Typeface = cached ?: synchronized(this) {
        cached ?: Typeface.CustomFallbackBuilder(
            FontFamily.Builder(Font.Builder(context.resources, R.font.nerd_symbols_mono).build()).build()
        ).setSystemFallback("monospace").build().also { cached = it }
    }
}
