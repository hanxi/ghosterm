package com.hanxi.ghosterm

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Color as AndroidColor
import androidx.compose.ui.graphics.toArgb
import androidx.core.graphics.PathParser
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.math.abs

/** Resource/compiler/rendering checks against the reviewed SVG/CSS, independent of UI state. */
@RunWith(AndroidJUnit4::class)
class AndroidDesignResourcesTest {
    private val instrumentation get()=InstrumentationRegistry.getInstrumentation()
    private fun reference()=JSONObject(instrumentation.context.assets.open("ui-reference.json").bufferedReader().use { it.readText() })
    private fun color(value:String):Int {
        val full=if(value.length==4) "#"+value.drop(1).flatMap { listOf(it,it) }.joinToString("") else value
        return AndroidColor.parseColor(full)
    }
    @Test fun nativeSemanticColorsMatchBothReviewedHtmlPalettes() {
        val themes=reference().getJSONObject("themes")
        for(dark in listOf(false,true)) {
            val scheme=ghostermColors(dark);val expected=themes.getJSONObject(if(dark) "dark" else "light")
            val roles=mapOf("bg" to scheme.background,"surface" to scheme.surface,"surface2" to scheme.surfaceContainerHigh,
                "surface3" to scheme.surfaceContainerHighest,"text" to scheme.onSurface,"secondary" to scheme.onSurfaceVariant,
                "primary" to scheme.primary,"on-primary" to scheme.onPrimary,"tint" to scheme.primaryContainer,"outline" to scheme.outline)
            for((name,actual) in roles) assertEquals("CSS role $name, dark=$dark",color(expected.getString(name)),actual.toArgb())
        }
    }
    @Test fun everyNativeVectorRendersTheReviewedSvgGeometryWithoutAnIconFont() {
        val source=reference();val paths=source.getJSONObject("paths")
        assertEquals(paths.length(),UiIcon.entries.size)
        for(icon in UiIcon.entries) {
            val name=icon.name.lowercase()
            val drawable=checkNotNull(instrumentation.targetContext.getDrawable(icon.resource))
            val actual=Bitmap.createBitmap(48,48,Bitmap.Config.ARGB_8888)
            drawable.setBounds(0,0,48,48);drawable.draw(Canvas(actual))
            val expected=Bitmap.createBitmap(48,48,Bitmap.Config.ARGB_8888)
            val canvas=Canvas(expected);canvas.scale(2f,2f)
            val paint=Paint(Paint.ANTI_ALIAS_FLAG).apply { style=Paint.Style.STROKE;color=AndroidColor.BLACK
                strokeWidth=source.getDouble("strokeWidth").toFloat();strokeCap=Paint.Cap.ROUND;strokeJoin=Paint.Join.ROUND }
            canvas.drawPath(checkNotNull(PathParser.createPathFromPathData(paths.getString(name))),paint)
            var difference=0L;var ink=0L
            for(y in 0 until 48) for(x in 0 until 48) {
                difference+=abs(AndroidColor.alpha(actual.getPixel(x,y))-AndroidColor.alpha(expected.getPixel(x,y)))
                ink+=AndroidColor.alpha(actual.getPixel(x,y))
            }
            assertTrue("Icon $name must draw actual geometry",ink>1000)
            assertTrue("Icon $name differs from SVG: mean alpha error ${difference/2304.0}",difference/2304.0<4.0)
            actual.recycle();expected.recycle()
        }
    }
}
