package com.hanxi.ghosterm

import android.app.Instrumentation
import android.graphics.Rect
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.accessibility.AccessibilityNodeInfo

/** Android 10 DocumentsUI list rows omit the clickable accessibility bit.
 * Use an actual touch for those visible, enabled system rows. Grant assertions
 * still inspect the URI and persisted permission returned by DocumentsUI.
 * null means an ordinary control; false means retry a stale legacy row. */
internal fun tapLegacyDocumentRow(instrumentation: Instrumentation, node: AccessibilityNodeInfo): Boolean? {
    if(android.os.Build.VERSION.SDK_INT>29 || node.packageName?.toString()?.contains("documentsui")!=true) return null
    var parent=node.parent
    var inList=false
    while(parent!=null) {
        val id=parent.viewIdResourceName
        if(parent.className?.toString()=="android.widget.ListView" ||
            id?.endsWith(":id/dir_list")==true || id?.endsWith(":id/roots_list")==true) { inList=true;break }
        parent=parent.parent
    }
    if(!inList) return null
    if(!node.isEnabled || !node.isVisibleToUser) return false
    val text=node.text?.toString();val description=node.contentDescription?.toString()
    // A scroll action returns before RecyclerView finishes moving/rebinding rows.
    // Wait for the system window, then reject a reused node instead of tapping
    // its old position or falling through to an unrelated ancestor's action.
    try { instrumentation.uiAutomation.waitForIdle(100,1500) }
    catch(_:java.util.concurrent.TimeoutException) { return false }
    if(!node.refresh() || !node.isEnabled || !node.isVisibleToUser ||
        node.text?.toString()!=text || node.contentDescription?.toString()!=description) return false
    val original=Rect();node.getBoundsInScreen(original)
    SystemClock.sleep(100)
    if(!node.refresh() || node.text?.toString()!=text || node.contentDescription?.toString()!=description) return false
    val bounds=Rect();node.getBoundsInScreen(bounds)
    if(bounds!=original) return false
    var ancestor=node.parent
    while(ancestor!=null) {
        val clip=Rect();ancestor.getBoundsInScreen(clip)
        if(!bounds.intersect(clip)) return false
        ancestor=ancestor.parent
    }
    val minimum=(16*instrumentation.targetContext.resources.displayMetrics.density).toInt().coerceAtLeast(1)
    if(bounds.isEmpty || bounds.height()<minOf(original.height(),minimum) || bounds.width()<minOf(original.width(),minimum)) return false
    val start=SystemClock.uptimeMillis()
    fun inject(action:Int):Boolean {
        val event=MotionEvent.obtain(start,SystemClock.uptimeMillis(),action,bounds.exactCenterX(),bounds.exactCenterY(),0)
        event.source=InputDevice.SOURCE_TOUCHSCREEN
        return try { instrumentation.uiAutomation.injectInputEvent(event,true) } finally { event.recycle() }
    }
    if(!inject(MotionEvent.ACTION_DOWN)) return false
    SystemClock.sleep(50)
    return inject(MotionEvent.ACTION_UP)
}
