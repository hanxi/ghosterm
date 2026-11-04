package com.hanxi.ghosterm

import android.os.Build
import android.view.WindowManager
import android.view.ViewTreeObserver
import androidx.activity.compose.LocalActivity
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

/** Own only the status bar while this terminal page is mounted. */
@Composable
internal fun TerminalSystemBars(visible: Boolean) {
    val activity = LocalActivity.current
    DisposableEffect(activity) {
        val window = activity?.window
        val controller = window?.let { WindowCompat.getInsetsController(it, it.decorView) }
        val previousVisible = window?.let { ViewCompat.getRootWindowInsets(it.decorView)
            ?.isVisible(WindowInsetsCompat.Type.statusBars()) } ?: true
        val previousBehavior = controller?.systemBarsBehavior
        val previousCutoutMode = window?.attributes?.layoutInDisplayCutoutMode
        onDispose {
            if (window != null) window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = checkNotNull(previousCutoutMode)
            }
            controller?.let {
                it.systemBarsBehavior = checkNotNull(previousBehavior)
                if (previousVisible) it.show(WindowInsetsCompat.Type.statusBars())
                else it.hide(WindowInsetsCompat.Type.statusBars())
            }
        }
    }
    DisposableEffect(activity, visible) {
        val window = activity?.window
        val decor = window?.decorView
        val originalCutoutMode = window?.attributes?.layoutInDisplayCutoutMode
        val applyVisibility = Runnable {
            if (window != null) {
                window.attributes = window.attributes.apply {
                    layoutInDisplayCutoutMode = if (visible) checkNotNull(originalCutoutMode)
                        else if (Build.VERSION.SDK_INT >= 30) WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                        else WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                }
                WindowCompat.getInsetsController(window, window.decorView).apply {
                    systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                    if (visible) show(WindowInsetsCompat.Type.statusBars())
                    else hide(WindowInsetsCompat.Type.statusBars())
                }
            }
        }
        // On older Android versions a new/rotated window can apply its decor
        // flags after composition. Reapply once it actually owns window focus.
        val focusListener = ViewTreeObserver.OnWindowFocusChangeListener { focused ->
            if (focused) decor?.post(applyVisibility)
        }
        decor?.viewTreeObserver?.addOnWindowFocusChangeListener(focusListener)
        decor?.post(applyVisibility)
        onDispose {
            if (window != null) window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = checkNotNull(originalCutoutMode)
            }
            decor?.removeCallbacks(applyVisibility)
            decor?.viewTreeObserver?.removeOnWindowFocusChangeListener(focusListener)
        }
    }
}
