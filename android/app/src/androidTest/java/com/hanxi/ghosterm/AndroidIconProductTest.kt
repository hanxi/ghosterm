package com.hanxi.ghosterm

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.drawable.AdaptiveIconDrawable
import android.graphics.drawable.ColorDrawable
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/** Validate the installed application's icon, including the actual adaptive foreground. */
@RunWith(AndroidJUnit4::class)
class AndroidIconProductTest {
    @Test fun packageManagerLoadsTheOfficialGhostAdaptiveIcon() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val info = context.packageManager.getApplicationInfo(context.packageName, 0)
        assertTrue("Application must reference a generated brand icon", info.icon in setOf(R.mipmap.ic_launcher, R.mipmap.ic_launcher_round))
        val drawable = context.packageManager.getApplicationIcon(context.packageName)
        assertTrue("Android 10+ must load the adaptive brand icon", drawable is AdaptiveIconDrawable)
        val icon = drawable as AdaptiveIconDrawable
        assertTrue(icon.background is ColorDrawable)
        assertEquals("Background comes from the SVG's gt-bg gradient", Color.rgb(10, 13, 28), (icon.background as ColorDrawable).color)
        val bitmap = Bitmap.createBitmap(192, 192, Bitmap.Config.ARGB_8888)
        try {
            icon.setBounds(0, 0, 192, 192); icon.draw(Canvas(bitmap))
            fun bright(x: Int, y: Int): Boolean = bitmap.getPixel(x, y).let { Color.red(it) > 190 && Color.green(it) > 190 && Color.blue(it) > 190 && Color.alpha(it) == 255 }
            fun eye(x: Int, y: Int): Boolean = bitmap.getPixel(x, y).let { Color.red(it) < 80 && Color.green(it) < 80 && Color.blue(it) < 120 && Color.alpha(it) == 255 }
            assertTrue("Official ghost body must be visible", bright(96, 96))
            assertTrue("White face between the two eyes must remain visible", bright(96, 70))
            assertTrue("Left ghost eye must remain visible", eye(72, 70))
            assertTrue("Right ghost eye must remain visible", eye(120, 70))
            File(context.cacheDir, "official-launcher-icon.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        } finally { bitmap.recycle() }
    }
}
