package com.hanxi.ghosterm

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import androidx.compose.foundation.layout.Row
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.core.graphics.PathParser
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import kotlin.math.abs

/** Exercise the actual shared composable, rather than only its resource table. */
class AndroidDesignComponentsTest {
    @get:Rule val ui=createComposeRule()
    @Test fun sharedIconPreservesRequestedGeometryAndThemeTint() {
        val dark=mutableStateOf(false)
        val icons=listOf(UiIcon.Keyboard,UiIcon.Folder,UiIcon.Settings)
        val paths=JSONObject(InstrumentationRegistry.getInstrumentation().context.assets.open("ui-reference.json")
            .bufferedReader().use { it.readText() }).getJSONObject("paths")
        ui.setContent { GhostermTheme(dark.value) { Row {
            icons.forEach { GhostermIcon(it,modifier=Modifier.testTag(it.name),tint=androidx.compose.material3.MaterialTheme.colorScheme.primary) }
        } } }
        for(theme in listOf(false,true)) {
            ui.runOnIdle { dark.value=theme };ui.waitForIdle()
            val colors=ghostermColors(theme)
            for(icon in icons) {
                val actual=ui.onNodeWithTag(icon.name).captureToImage().asAndroidBitmap()
                val expected=Bitmap.createBitmap(actual.width,actual.height,Bitmap.Config.ARGB_8888)
                val canvas=Canvas(expected);canvas.drawColor(colors.background.toArgb());canvas.scale(actual.width/24f,actual.height/24f)
                val paint=Paint(Paint.ANTI_ALIAS_FLAG).apply { style=Paint.Style.STROKE;color=colors.primary.toArgb();strokeWidth=1.7f;strokeCap=Paint.Cap.ROUND;strokeJoin=Paint.Join.ROUND }
                canvas.drawPath(checkNotNull(PathParser.createPathFromPathData(paths.getString(icon.name.lowercase()))),paint)
                var difference=0L
                for(y in 0 until actual.height) for(x in 0 until actual.width) {
                    val a=actual.getPixel(x,y);val b=expected.getPixel(x,y)
                    for(shift in listOf(0,8,16)) difference+=abs((a shr shift and 255)-(b shr shift and 255))
                }
                assertTrue("Shared ${icon.name}, dark=$theme must match SVG and primary tint: $difference",difference.toDouble()/(actual.width*actual.height*3)<5)
                expected.recycle()
            }
        }
    }
    @Test fun sharedIconButtonHasAccessibleLabelAndMinimumTouchTarget() {
        var clicks=0
        ui.setContent { GhostermTheme { GhostermIconButton(UiIcon.Keyboard,"切换键盘",{ clicks++ }) } }
        ui.onNodeWithContentDescription("切换键盘").assertHasClickAction().assertWidthIsAtLeast(androidx.compose.ui.unit.Dp(48f))
            .assertHeightIsAtLeast(androidx.compose.ui.unit.Dp(48f)).performClick()
        ui.runOnIdle { assertEquals(1,clicks) }
    }
}
