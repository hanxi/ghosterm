package com.hanxi.ghosterm

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Semantic roles mapped from docs/design/android-prototype.html, variant b. */
internal fun ghostermColors(dark: Boolean): ColorScheme {
    val bg=if(dark) Color(0xFF171B20) else Color(0xFFF7F8FC)
    val surface=if(dark) Color(0xFF21262D) else Color.White
    val surface2=if(dark) Color(0xFF262D34) else Color(0xFFEDF0F7)
    val surface3=if(dark) Color(0xFF333C46) else Color(0xFFDEE5F2)
    val text=if(dark) Color(0xFFE4E8EE) else Color(0xFF232C3B)
    val secondary=if(dark) Color(0xFFA5AFB9) else Color(0xFF626E82)
    val primary=if(dark) Color(0xFFBACDFF) else Color(0xFF425F96)
    val onPrimary=if(dark) Color(0xFF1B2E55) else Color.White
    val tint=if(dark) Color(0xFF334567) else Color(0xFFDCE6FF)
    val outline=if(dark) Color(0xFF3B434E) else Color(0xFFD5DCE9)
    val error=if(dark) Color(0xFFFFB4AB) else Color(0xFF9E3630)
    val errorContainer=if(dark) Color(0xFF542525) else Color(0xFFFFDAD5)
    // Set every role used by Material components: no unrelated default purple.
    val base=if(dark) darkColorScheme() else lightColorScheme()
    return base.copy(primary=primary,onPrimary=onPrimary,primaryContainer=tint,onPrimaryContainer=text,
        inversePrimary=if(dark) Color(0xFF425F96) else Color(0xFFBACDFF),
        secondary=secondary,onSecondary=bg,secondaryContainer=surface2,onSecondaryContainer=text,
        tertiary=primary,onTertiary=onPrimary,tertiaryContainer=surface3,onTertiaryContainer=text,
        background=bg,onBackground=text,surface=surface,onSurface=text,surfaceVariant=surface2,
        onSurfaceVariant=secondary,surfaceTint=primary,inverseSurface=text,inverseOnSurface=bg,
        error=error,onError=if(dark) Color(0xFF381510) else Color.White,
        errorContainer=errorContainer,onErrorContainer=if(dark) Color(0xFFFFDAD5) else Color(0xFF381510),
        outline=outline,outlineVariant=outline.copy(alpha=.6f),scrim=Color.Black,
        surfaceBright=surface3,surfaceDim=bg,surfaceContainerLowest=bg,surfaceContainerLow=surface,
        surfaceContainer=surface,surfaceContainerHigh=surface2,surfaceContainerHighest=surface3,
        primaryFixed=Color(0xFFDCE6FF),primaryFixedDim=Color(0xFFBACDFF),onPrimaryFixed=Color(0xFF1B2E55),onPrimaryFixedVariant=Color(0xFF425F96),
        secondaryFixed=Color(0xFFEDF0F7),secondaryFixedDim=Color(0xFFDEE5F2),onSecondaryFixed=Color(0xFF232C3B),onSecondaryFixedVariant=Color(0xFF626E82),
        tertiaryFixed=Color(0xFFDEE5F2),tertiaryFixedDim=Color(0xFFBACDFF),onTertiaryFixed=Color(0xFF1B2E55),onTertiaryFixedVariant=Color(0xFF425F96))
}

private fun text(size: Int, height: Int, weight: Int=400)=TextStyle(
    fontFamily=FontFamily.SansSerif,fontSize=size.sp,lineHeight=height.sp,fontWeight=FontWeight(weight))
internal val GhostermTypography=Typography(
    displayLarge=text(36,44,550),displayMedium=text(34,42,550),displaySmall=text(28,36,500),
    headlineLarge=text(34,42,650),headlineMedium=text(28,36,500),headlineSmall=text(21,28,550),
    titleLarge=text(19,26,600),titleMedium=text(15,22,600),titleSmall=text(13,20,600),
    bodyLarge=text(14,22),bodyMedium=text(13,20),bodySmall=text(11,18),
    labelLarge=text(13,20,550),labelMedium=text(11,16,550),labelSmall=text(10,15))
internal val GhostermShapes=Shapes(extraSmall=RoundedCornerShape(8.dp),small=RoundedCornerShape(12.dp),
    medium=RoundedCornerShape(18.dp),large=RoundedCornerShape(20.dp),extraLarge=RoundedCornerShape(28.dp))
internal object GhostermDimensions {
    val page=18.dp
    val icon=22.dp
    val touch=48.dp
    val row=76.dp
}

@Composable
internal fun GhostermTheme(dark: Boolean=isSystemInDarkTheme(), content: @Composable () -> Unit) {
    MaterialTheme(colorScheme=ghostermColors(dark),typography=GhostermTypography,shapes=GhostermShapes) {
        Surface(color=MaterialTheme.colorScheme.background,content=content)
    }
}
