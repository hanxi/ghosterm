package com.hanxi.ghosterm.terminal

import android.content.Context
import android.content.ClipData
import android.content.ClipboardManager
import android.app.AlertDialog
import android.view.ActionMode
import android.view.Menu
import android.view.MenuItem
import android.view.accessibility.AccessibilityNodeInfo
import android.view.accessibility.AccessibilityEvent
import android.os.Bundle
import kotlinx.coroutines.*
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.Typeface
import android.os.SystemClock
import android.util.TypedValue
import android.text.Editable
import android.text.InputType
import android.text.Selection
import android.text.SpannableStringBuilder
import android.view.GestureDetector
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.CursorAnchorInfo
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import android.view.inputmethod.SurroundingText
import kotlin.math.ceil

/** Canvas consumes cell snapshots; neither PTY output nor transport types enter this View. */
class TerminalView(context: Context) : View(context) {
    var surface: TerminalSurface? = null
        set(value) {
            if (field === value) return
            releaseHeldKeys()
            resetTextActions()
            connection?.cancel(); connection = null
            field?.visible(false);field = value;value?.visible(running);currentGraphics=null; currentFrame = null; lastSize = null; inputFrameDeadline = 0L
            ctrl = 0; updateSize(); invalidate()
            inputMethod.restartInput(this)
        }
    var onError: (String) -> Unit = {}
    var onModifiersChanged: (Int) -> Unit = {}
    /** 0 off, 1 one-shot, 2 locked. */
    var ctrl: Int = 0
        set(value) { field = value; onModifiersChanged(value) }
    private var auxiliaryKeys = true
    private var lockedCtrl = false
    private var awakeRequested = false
    private var automaticKeyboardUntil = 0L
    private val automaticKeyboard = object : Runnable {
        override fun run() {
            if (automaticKeyboardUntil == 0L || SystemClock.uptimeMillis() > automaticKeyboardUntil) {
                automaticKeyboardUntil = 0L; return
            }
            if (!isAttachedToWindow || !isShown || windowVisibility != VISIBLE) {
                cancelAutomaticKeyboard(); return
            }
            if (hasWindowFocus()) { automaticKeyboardUntil = 0L; showKeyboardNow() }
            else postDelayed(this, 25)
        }
    }
    fun applyAuxiliaryPolicy(visible: Boolean, locked: Boolean) {
        if (auxiliaryKeys == visible && lockedCtrl == locked) return
        auxiliaryKeys = visible; lockedCtrl = locked; ctrl = 0
    }
    fun toggleCtrl() {
        ctrl = if (!auxiliaryKeys || ctrl != 0) 0 else if (lockedCtrl) 2 else 1
    }
    fun requestKeepAwake(enabled: Boolean) { awakeRequested = enabled; updateKeepAwake() }
    private fun updateKeepAwake() {
        keepScreenOn = awakeRequested && isAttachedToWindow && isShown && windowVisibility == VISIBLE && hasWindowFocus()
    }
    fun requestAutomaticKeyboard() {
        automaticKeyboardUntil = SystemClock.uptimeMillis() + 1_000
        removeCallbacks(automaticKeyboard); post(automaticKeyboard)
    }
    private fun cancelAutomaticKeyboard() { automaticKeyboardUntil = 0L; removeCallbacks(automaticKeyboard) }
    var currentFrame: TerminalFrame? = null
        private set
    private var imeComposition = ""
    private var imeUnderlined = false
    private val pendingAccents = mutableMapOf<Int, Int>()
    val composition: String get() = imeComposition.ifEmpty {
        pendingAccents.values.lastOrNull()?.let { String(Character.toChars(it)) } ?: ""
    }
    private var connection: TerminalInputConnection? = null
    private data class HeldKey(val code: Int, val modifiers: Int, val unshifted: Int, val softCtrl: Int)
    private val heldKeys = mutableMapOf<Pair<Int, Int>, HeldKey>()
    private fun releaseHeldKeys() {
        pendingAccents.clear(); invalidate()
        val pending = heldKeys.values.toList()
        heldKeys.clear()
        pending.forEach { key -> dispatch {
            surface?.key(key.code, key.modifiers, "", key.unshifted, KeyAction.RELEASE)
        } }
    }
    private var actionScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var actionMode: ActionMode? = null
    private var pasteDialog: AlertDialog? = null
    private var selecting = false
    private var dragging: SelectionOperation? = null
    private var dragPoint: Pair<Int, Int>? = null
    private var dragJob: Job? = null
    private var dragScroll = 0
    private var orderDrag = false
    private val clipboard get() = context.getSystemService(ClipboardManager::class.java)
    private val handleRadius get() = 10f * resources.displayMetrics.density
    private val inputMethod get() = context.getSystemService(InputMethodManager::class.java)
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = TerminalFonts.typeface(context) }
    // Symbol fallback must not change the text grid or the baseline of regular text.
    private val metricsPaint = Paint().apply { typeface = Typeface.MONOSPACE }
    private val fill = Paint()
    private var cellWidth = 1
    private var cellHeight = 1
    private var baseline = 1f
    private var metricsReady = false
    var style: TerminalStyle = TerminalStyle()
        private set
    private var lastSize: List<Int>? = null
    private var running = false
    private var inputFrameDeadline = 0L
    private var blink = true
    private var fractionalScroll = 0f
    private var pointerContextClick = false
    private val gestures = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDown(event: MotionEvent): Boolean { pointerContextClick = false; return true }
        override fun onSingleTapUp(event: MotionEvent): Boolean {
            if (pointerContextClick) { pointerContextClick = false; return true }
            if (selecting) dismissSelection() else tapTerminal(event, 1)
            return true
        }
        override fun onContextClick(event: MotionEvent): Boolean {
            pointerContextClick = true
            if (selecting) dismissSelection() else tapTerminal(event, 2)
            return true
        }
        override fun onLongPress(event: MotionEvent) {
            val frame = currentFrame ?: return
            beginSelection((event.x / cellWidth).toInt().coerceIn(0, frame.cols - 1),
                (event.y / cellHeight).toInt().coerceIn(0, frame.rows - 1))
        }
        override fun onScroll(first: MotionEvent?, last: MotionEvent, dx: Float, dy: Float): Boolean {
            fractionalScroll += dy
            val lines = (fractionalScroll / cellHeight).toInt()
            if (lines != 0) { dispatch { surface?.scroll(lines) }; fractionalScroll -= lines * cellHeight }
            return true
        }
    })
    private var currentGraphics: TerminalGraphics?=null
    private val imagePaint=Paint(Paint.FILTER_BITMAP_FLAG)
    private val tick = object : Runnable {
        override fun run() {
            if (!running) return
            dispatch {
                surface?.graphics(currentGraphics?.revision ?: 0)?.let { currentGraphics=it;invalidate() }
                surface?.frame(currentFrame?.revision ?: 0)?.let {
                    currentFrame = it; invalidate(); connection?.anchor(); actionMode?.invalidateContentRect()
                }
            }
            val next = SystemClock.uptimeMillis() % 1000 < 500
            if (next != blink) {
                blink = next
                if ((style.appearance.blink && hasFocus() && hasWindowFocus() && currentFrame?.cursorBlink == true) || currentFrame?.hasBlinkingText == true) invalidate()
            }
            // Input needs a prompt echo; sustained output and idle retain the
            // lower polling rate to avoid rebuilding snapshots every display frame.
            if (SystemClock.uptimeMillis() < inputFrameDeadline) postOnAnimation(this)
            else postDelayed(this, 32)
        }
    }
    init {
        isFocusable = true; isFocusableInTouchMode = true
        // The terminal paints its own focus. Framework highlights tint ANSI colors.
        defaultFocusHighlightEnabled = false
        isLongClickable = true
        contentDescription = "终端"
        importantForAutofill = IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS
        updateStyle(style)
    }
    fun setFontSize(sp: Float) {
        require(sp in 8f..32f)
        updateStyle(style.copy(fontSize = sp))
    }
    fun updateStyle(value: TerminalStyle) {
        val pixels = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, value.fontSize, resources.displayMetrics)
        val changedMetrics = !metricsReady || style.fontSize != value.fontSize || style.lineHeight != value.lineHeight || paint.textSize != pixels
        if (style == value && !changedMetrics) return
        style = value
        if (changedMetrics) {
            paint.textSize = pixels
            metricsPaint.textSize = pixels
            cellWidth = ceil(metricsPaint.measureText("M")).toInt().coerceAtLeast(1)
            cellHeight = ceil(paint.textSize * value.lineHeight).toInt().coerceAtLeast(1)
            baseline = (cellHeight - (metricsPaint.fontMetrics.descent - metricsPaint.fontMetrics.ascent)) / 2 - metricsPaint.fontMetrics.ascent
            metricsReady = true
            lastSize = null; updateSize()
        }
        invalidate()
    }
    private fun updateSize() {
        val target = surface ?: return
        if (width < cellWidth || height < cellHeight) return
        val size = listOf((width / cellWidth).coerceAtMost(256), (height / cellHeight).coerceAtMost(256), cellWidth, cellHeight)
        if (lastSize != size && dispatch { target.resize(size[0], size[1], size[2], size[3]) }) lastSize = size
    }
    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) { updateSize() }
    private fun dispatch(action: () -> Unit): Boolean = try { action(); true } catch (e: IllegalStateException) {
        onError(e.message ?: "终端操作失败"); false
    }
    private fun dispatchInput(action: () -> Unit): Boolean = dispatch(action).also { sent ->
        if (sent && running) {
            inputFrameDeadline = SystemClock.uptimeMillis() + 120
            removeCallbacks(tick)
            postOnAnimation(tick)
        }
    }
    private fun rendering() {
        updateKeepAwake()
        val next = isAttachedToWindow && isShown && windowVisibility == VISIBLE
        if (next == running) return
        running = next
        dispatch { surface?.visible(next) }
        if (running) post(tick) else removeCallbacks(tick)
    }
    override fun onAttachedToWindow() {
        super.onAttachedToWindow(); actionScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); rendering()
        textAction { it.select(SelectionOperation.CLEAR) }
        // AndroidView is created before it joins the focus tree. Request once
        // attached, without showing the IME or stealing focus from a later page.
        post { if (isAttachedToWindow && isShown) requestFocus() }
    }
    override fun onDetachedFromWindow() {
        cancelAutomaticKeyboard(); keepScreenOn = false
        releaseHeldKeys()
        resetTextActions()
        ctrl = 0
        dispatch { surface?.visible(false) }
        running = false; removeCallbacks(tick); connection?.cancel(); connection = null
        super.onDetachedFromWindow()
    }
    override fun onVisibilityAggregated(visible: Boolean) { super.onVisibilityAggregated(visible); rendering() }
    override fun onWindowVisibilityChanged(visibility: Int) { super.onWindowVisibilityChanged(visibility); rendering() }
    override fun onFocusChanged(gain: Boolean, direction: Int, previous: Rect?) {
        super.onFocusChanged(gain, direction, previous)
        if (!gain) releaseHeldKeys()
        if (!gain) { stopDrag(); connection?.cancel(); connection = null; ctrl = 0 }
        invalidate()
    }
    override fun onWindowFocusChanged(hasWindowFocus: Boolean) {
        super.onWindowFocusChanged(hasWindowFocus)
        if (!hasWindowFocus) releaseHeldKeys()
        if (!hasWindowFocus) { stopDrag(); ctrl = 0 }
        updateKeepAwake()
        invalidate()
    }
    override fun performClick(): Boolean {
        super.performClick(); showKeyboard(); return true
    }
    private fun tapTerminal(event: MotionEvent, button: Int) {
        val frame = currentFrame ?: run { performClick(); return }
        val x = event.x.toInt().coerceIn(0, frame.cols * cellWidth - 1)
        val y = event.y.toInt().coerceIn(0, frame.rows * cellHeight - 1)
        val modifiers = (if (event.metaState and KeyEvent.META_SHIFT_MASK != 0) 1 else 0) or
            (if (event.metaState and KeyEvent.META_CTRL_MASK != 0 || ctrl != 0) 2 else 0) or
            (if (event.metaState and KeyEvent.META_ALT_MASK != 0) 4 else 0)
        textAction { target ->
            if (target.click(x, y, modifiers, button)) {
                super.performClick()
                requestFocus()
                if (ctrl == 1) ctrl = 0
                inputFrameDeadline = SystemClock.uptimeMillis() + 120
                removeCallbacks(tick); postOnAnimation(tick)
            } else if (surface === target && isAttachedToWindow && isShown) performClick()
        }
    }
    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_BUTTON_PRESS && event.actionButton == MotionEvent.BUTTON_TERTIARY) {
            pointerContextClick = true
            if (selecting) dismissSelection() else tapTerminal(event, 3)
            return true
        }
        return gestures.onGenericMotionEvent(event) || super.onGenericMotionEvent(event)
    }
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_DOWN && selecting) {
            stopDrag()
            val bounds = selectionHandles()
            dragging = bounds?.let { (start, end) ->
                listOf(SelectionOperation.START to start, SelectionOperation.END to end)
                    .map { (operation, point) -> operation to kotlin.math.hypot(event.x - point.first, event.y - point.second) }
                    .minByOrNull { it.second }?.takeIf { it.second < handleRadius * 2.4f }?.first
            }
            orderDrag = dragging != null
        }
        if (dragging != null) {
            if (event.actionMasked == MotionEvent.ACTION_MOVE) {
                val frame = currentFrame ?: return true
                dragPoint = (event.x / cellWidth).toInt().coerceIn(0, frame.cols - 1) to
                    (event.y / cellHeight).toInt().coerceIn(0, frame.rows - 1)
                val edge = minOf(height / 4f, maxOf(cellHeight.toFloat(), 24f * resources.displayMetrics.density))
                dragScroll = when {
                    event.y < edge -> -1
                    event.y > height - edge -> 1
                    else -> 0
                }
                updateDrag()
            }
            if (event.actionMasked == MotionEvent.ACTION_UP) finishDrag()
            if (event.actionMasked == MotionEvent.ACTION_CANCEL) stopDrag()
            return true
        }
        return gestures.onTouchEvent(event)
    }
    private fun updateDrag() {
        val operation = dragging ?: return
        if (dragJob?.isActive == true) return
        dragJob = textAction { target ->
            if (orderDrag) { orderDrag = false; target.select(SelectionOperation.ORDER) }
            while (dragPoint != null) {
                val point = dragPoint!!; dragPoint = null
                // One acknowledged native operation keeps viewport and endpoint
                // consistent even when this coroutine is cancelled mid-request.
                target.extendSelection(operation, point.first, point.second, if (dragging == operation) dragScroll else 0)
                if (dragScroll == 0 || dragging != operation) continue
                delay(80)
                if (dragging == operation && dragPoint == null) dragPoint = point
            }
        }
    }
    private fun finishDrag() {
        dragging = null; dragScroll = 0
        // Drain an already pending final move, but never schedule another scroll.
    }
    private fun stopDrag() {
        finishDrag(); dragPoint = null; orderDrag = false
        dragJob?.cancel(); dragJob = null
    }
    private fun resetTextActions() {
        stopDrag(); selecting = false
        pasteDialog?.dismiss(); pasteDialog = null
        actionMode?.finish(); actionMode = null
        actionScope.cancel()
        actionScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    }
    private fun textAction(action: suspend (TerminalSurface) -> Unit): Job {
        val target = surface
        return actionScope.launch {
            if (target == null) return@launch
            try { action(target) }
            catch (cancel: CancellationException) { throw cancel }
            catch (failure: IllegalStateException) { onError(failure.message ?: "文本操作失败") }
        }
    }
    private fun beginSelection(x: Int, y: Int) {
        stopDrag(); selecting = true; dragging = SelectionOperation.END; orderDrag = true
        performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS)
        textAction { target -> target.select(SelectionOperation.WORD, x, y); showTextActions() }
    }
    fun dismissSelection(): Boolean {
        if (!selecting) return false
        selecting = false; stopDrag()
        actionMode?.finish(); actionMode = null
        textAction { it.select(SelectionOperation.CLEAR) }
        invalidate(); return true
    }
    private fun selectionHandles(): Pair<Pair<Float, Float>, Pair<Float, Float>>? {
        val frame = currentFrame ?: return null
        val first = frame.cells.indexOfFirst { it.flags and 256 != 0 }
        val last = frame.cells.indexOfLast { it.flags and 256 != 0 }
        if (first < 0) return null
        fun position(index: Int, after: Boolean) =
            ((index % frame.cols + if (after) 1 else 0) * cellWidth).toFloat().coerceIn(handleRadius, (width - handleRadius).coerceAtLeast(handleRadius)) to
                ((index / frame.cols + 1) * cellHeight).toFloat().coerceIn(handleRadius, (height - handleRadius).coerceAtLeast(handleRadius))
        return position(first, false) to position(last, true)
    }
    private fun showTextActions() {
        if (!isAttachedToWindow || !selecting) return
        if (actionMode != null) { actionMode?.invalidate(); return }
        actionMode = startActionMode(object : ActionMode.Callback2() {
            override fun onCreateActionMode(mode: ActionMode, menu: Menu): Boolean {
                menu.add(0, android.R.id.copy, 0, "复制").setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS)
                menu.add(0, android.R.id.paste, 1, "粘贴").setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS)
                menu.add(0, android.R.id.selectAll, 2, "全选").setShowAsAction(MenuItem.SHOW_AS_ACTION_IF_ROOM)
                return true
            }
            override fun onPrepareActionMode(mode: ActionMode, menu: Menu) = false
            override fun onActionItemClicked(mode: ActionMode, item: MenuItem): Boolean {
                when (item.itemId) {
                    android.R.id.copy -> copySelection()
                    android.R.id.paste -> pasteClipboard()
                    android.R.id.selectAll -> textAction { it.select(SelectionOperation.ALL) }
                    else -> return false
                }
                return true
            }
            override fun onDestroyActionMode(mode: ActionMode) {
                actionMode = null
                if (selecting) dismissSelection()
            }
            override fun onGetContentRect(mode: ActionMode, view: View, out: Rect) {
                val handles = selectionHandles()
                if (handles == null) out.set(0, 0, width, cellHeight)
                else out.set(handles.first.first.toInt(), (handles.first.second - cellHeight).toInt(),
                    maxOf(handles.first.first.toInt() + cellWidth, handles.second.first.toInt()), handles.second.second.toInt())
            }
        }, ActionMode.TYPE_FLOATING)
    }
    fun copySelection() {
        textAction { target ->
            val text = target.copySelection()
            if (text.isEmpty()) { onError("没有选中的文本"); return@textAction }
            clipboard.setPrimaryClip(ClipData.newPlainText("终端文本", text))
            dismissSelection()
        }
    }
    fun clearLocal() { textAction { it.clearLocal(); dismissSelection() } }
    fun pasteClipboard() {
        val clip = clipboard.primaryClip
        val text = if (clip?.itemCount == 1 && clip.description.hasMimeType("text/*")) clip.getItemAt(0).text?.toString() else null
        if (text == null) { onError("剪贴板中没有可粘贴的文本"); return }
        pasteSnapshot(text, false)
    }
    private fun pasteSnapshot(text: String, confirmed: Boolean) {
        if (pasteDialog != null) return
        textAction { target ->
            if (target.paste(text, confirmed)) { dismissSelection(); return@textAction }
            if (!isAttachedToWindow || surface !== target) return@textAction
            pasteDialog = AlertDialog.Builder(context).setTitle("粘贴可能执行命令")
                .setMessage("内容含换行或终端控制序列，可能直接执行命令。\n\n" + text.take(500))
                .setNegativeButton("取消", null)
                .setPositiveButton("仍然粘贴") { _, _ -> pasteDialog = null; pasteSnapshot(text, true) }
                .create().also { dialog -> dialog.setOnDismissListener { if (pasteDialog === dialog) pasteDialog = null }; dialog.show() }
        }
    }
    override fun performLongClick(): Boolean {
        val frame = currentFrame ?: return false
        beginSelection(frame.cursorX.coerceIn(0, frame.cols - 1), frame.cursorY.coerceIn(0, frame.rows - 1))
        dragging = null
        return true
    }
    override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(info)
        info.className = android.widget.TextView::class.java.name
        info.isEditable = false
        info.contentDescription = null
        info.text = currentFrame?.accessibilityText() ?: "终端屏幕尚未就绪"
        info.addAction(AccessibilityNodeInfo.AccessibilityAction(AccessibilityNodeInfo.ACTION_LONG_CLICK, "选择终端文本"))
        info.addAction(AccessibilityNodeInfo.AccessibilityAction(android.R.id.paste, "粘贴文本"))
    }
    override fun onInitializeAccessibilityEvent(event: AccessibilityEvent) {
        super.onInitializeAccessibilityEvent(event)
        event.className = android.widget.TextView::class.java.name
    }
    override fun onPopulateAccessibilityEvent(event: AccessibilityEvent) {
        super.onPopulateAccessibilityEvent(event)
        if (event.eventType == AccessibilityEvent.TYPE_VIEW_FOCUSED ||
            event.eventType == AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED) {
            event.contentDescription = null
            event.text.clear()
            event.text.add(currentFrame?.accessibilityText() ?: "终端屏幕尚未就绪")
        }
    }
    override fun performAccessibilityAction(action: Int, args: Bundle?): Boolean {
        if (action == android.R.id.paste) { pasteClipboard(); return true }
        return super.performAccessibilityAction(action, args)
    }
    private fun showKeyboardNow() { requestFocus(); inputMethod.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT) }
    fun showKeyboard() { cancelAutomaticKeyboard(); showKeyboardNow() }
    fun hideKeyboard() { cancelAutomaticKeyboard(); inputMethod.hideSoftInputFromWindow(windowToken, 0) }
    fun sendKey(code: Int, modifiers: Int = 0, text: String = "", unshiftedCodepoint: Int = 0): Boolean {
        if (pendingAccents.isNotEmpty()) {
            if (code == KeyEvent.KEYCODE_ESCAPE || code == KeyEvent.KEYCODE_DEL) {
                pendingAccents.clear(); invalidate(); return true
            }
            val accents = pendingAccents.values.joinToString("") { String(Character.toChars(it)) }
            if (!dispatch { surface?.commit(accents) }) return false
            pendingAccents.clear(); invalidate()
        }
        val effectiveMods = modifiers or if (ctrl != 0) 2 else 0
        val sent = dispatchInput { surface?.let { target ->
            target.key(code, effectiveMods, text, unshiftedCodepoint, KeyAction.PRESS)
            target.key(code, effectiveMods, "", unshiftedCodepoint, KeyAction.RELEASE)
        } }
        if (sent) connection?.terminalKeyAccepted()
        if (sent && ctrl == 1) ctrl = 0
        return sent
    }
    fun commitText(text: String): Boolean {
        if (!validInputText(text)) return false
        pendingAccents.clear(); invalidate()
        if (ctrl != 0 && text.length == 1 && text[0] in ' '..'~') {
            // A soft keyboard commits characters, not physical key positions.
            // Let VT decide C0/CSI-u/modifyOtherKeys; never encode Ctrl bytes here.
            val character = text[0].lowercaseChar()
            val code = if (character in 'a'..'z') KeyEvent.KEYCODE_A + character.code - 'a'.code else KeyEvent.KEYCODE_UNKNOWN
            return sendKey(code, text = text, unshiftedCodepoint = character.code)
        }
        val sent = dispatchInput { surface?.commit(text) }
        if (sent && ctrl == 1) ctrl = 0
        return sent
    }
    private fun validInputText(text: CharSequence): Boolean {
        var index = 0
        while (index < text.length) {
            val character = text[index++]
            if (Character.isHighSurrogate(character)) {
                if (index < text.length && Character.isLowSurrogate(text[index])) { index++; continue }
            } else if (!Character.isLowSurrogate(character)) continue
            onError("输入文字不完整，请重新输入")
            return false
        }
        return true
    }
    override fun onCheckIsTextEditor() = true
    override fun onCreateInputConnection(info: EditorInfo): InputConnection {
        connection?.cancel()
        info.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        info.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING or EditorInfo.IME_ACTION_NONE
        info.initialSelStart = 0; info.initialSelEnd = 0
        return TerminalInputConnection().also { connection = it }
    }
    override fun onKeyDown(code: Int, event: KeyEvent): Boolean {
        if ((event.isCtrlPressed && event.isShiftPressed && code == KeyEvent.KEYCODE_V) ||
            (event.isShiftPressed && code == KeyEvent.KEYCODE_INSERT)) { pasteClipboard(); return true }
        if (event.isCtrlPressed && event.isShiftPressed && code == KeyEvent.KEYCODE_C) { copySelection(); return true }
        if (selecting && code == KeyEvent.KEYCODE_ESCAPE) { dismissSelection(); return true }
        if (selecting && event.isCtrlPressed && code == KeyEvent.KEYCODE_A) { textAction { it.select(SelectionOperation.ALL) }; return true }
        if (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_VOLUME_DOWN || code == KeyEvent.KEYCODE_VOLUME_UP)
            return super.onKeyDown(code, event)
        // A physical Return can arrive here directly while the IME keeps preedit.
        // Accept that text before sending Return through the terminal key encoder.
        if (code == KeyEvent.KEYCODE_ENTER && composition.isNotEmpty() && connection?.acceptForReturn() == false) return false
        val modifierKey = KeyEvent.isModifierKey(code) || code in setOf(
            KeyEvent.KEYCODE_CAPS_LOCK, KeyEvent.KEYCODE_NUM_LOCK, KeyEvent.KEYCODE_SCROLL_LOCK)
        val unicode = event.getUnicodeChar(event.metaState and KeyEvent.META_CTRL_MASK.inv() and KeyEvent.META_META_MASK.inv())
        if (!modifierKey) composeHardware(code, event, unicode)?.let { return it }
        // Ctrl/Meta combinations are shortcuts even when Alt alone is a dead key.
        val character = if (unicode and KeyCharacterMap.COMBINING_ACCENT != 0)
            event.getUnicodeChar(event.metaState and KeyEvent.META_CTRL_MASK.inv() and KeyEvent.META_META_MASK.inv() and KeyEvent.META_ALT_MASK.inv()) else unicode
        val text = if (character > 0) String(Character.toChars(character)) else ""
        val identity = event.deviceId to code
        val previous = heldKeys[identity]
        val softCtrl = if (modifierKey) 0 else previous?.softCtrl ?: if (ctrl != 0) 2 else 0
        val modifiers = keyModifiers(event) or softCtrl
        val unshifted = event.getUnicodeChar(0).let {
            if (it and android.view.KeyCharacterMap.COMBINING_ACCENT == 0) it else 0
        }
        val action = if (previous != null && event.repeatCount > 0) KeyAction.REPEAT else KeyAction.PRESS
        // Freeze keypad navigation identity until release, even if Num Lock changes.
        val terminalCode = previous?.code ?: if (!event.isNumLockOn &&
            (code in KeyEvent.KEYCODE_NUMPAD_0..KeyEvent.KEYCODE_NUMPAD_9 || code == KeyEvent.KEYCODE_NUMPAD_DOT))
            code or TerminalKeys.KEYPAD_NAVIGATION else code
        val sent = dispatchInput { surface?.key(terminalCode, modifiers, text, unshifted, action) }
        if (sent) {
            heldKeys[identity] = HeldKey(terminalCode, modifiers, unshifted, softCtrl)
            if (!modifierKey) connection?.terminalKeyAccepted()
            if (!modifierKey && ctrl == 1) ctrl = 0
        }
        return sent
    }
    /** Android resolves layout text; VT resolves terminal key protocols. */
    private fun composeHardware(code: Int, event: KeyEvent, unicode: Int): Boolean? {
        val device = event.deviceId
        val accent = pendingAccents[device]
        if (accent != null && (code == KeyEvent.KEYCODE_ESCAPE || code == KeyEvent.KEYCODE_DEL)) {
            pendingAccents.remove(device); invalidate(); return true
        }
        val dead = unicode and KeyCharacterMap.COMBINING_ACCENT != 0
        if (dead && !event.isCtrlPressed && !event.isMetaPressed) {
            val next = unicode and KeyCharacterMap.COMBINING_ACCENT_MASK
            if (accent != null) {
                val combined = KeyCharacterMap.getDeadChar(accent, next)
                if (combined != 0) { pendingAccents.remove(device); return commitHardware(String(Character.toChars(combined))) }
                if (!dispatch { surface?.commit(String(Character.toChars(accent))) }) return false
            }
            pendingAccents.remove(device); pendingAccents[device] = next; invalidate(); return true
        }
        if (accent != null) {
            pendingAccents.remove(device); invalidate()
            if (unicode >= 32 && unicode != 127 && !event.isCtrlPressed && !event.isMetaPressed) {
                val combined = KeyCharacterMap.getDeadChar(accent, unicode)
                val prefix = String(Character.toChars(accent))
                return commitHardware(if (combined != 0) String(Character.toChars(combined)) else prefix + String(Character.toChars(unicode)))
            }
            if (!dispatch { surface?.commit(String(Character.toChars(accent))) }) return false
        }
        // A layout-specific Alt character is text (e.g. virtual Alt+C → ç),
        // while an unchanged Alt character remains a terminal shortcut.
        if (!dead && unicode >= 32 && unicode != 127 && event.isAltPressed && !event.isCtrlPressed && !event.isMetaPressed &&
            unicode != event.getUnicodeChar(event.metaState and KeyEvent.META_ALT_MASK.inv())) {
            return commitHardware(String(Character.toChars(unicode)))
        }
        return null
    }
    private fun commitHardware(text: String): Boolean {
        invalidate()
        val sent = dispatchInput { surface?.commit(text) }
        if (sent && ctrl == 1) ctrl = 0
        return sent
    }
    private fun keyModifiers(event: KeyEvent): Int =
        (if (event.isShiftPressed) 1 else 0) or (if (event.isCtrlPressed) 2 else 0) or
            (if (event.isAltPressed) 4 else 0) or (if (event.isMetaPressed) 8 else 0) or
            (if (event.isCapsLockOn) 16 else 0) or (if (event.isNumLockOn) 32 else 0) or
            (if (event.isShiftPressed && event.metaState and KeyEvent.META_SHIFT_RIGHT_ON != 0) 64 else 0) or
            (if (event.isCtrlPressed && event.metaState and KeyEvent.META_CTRL_RIGHT_ON != 0) 128 else 0) or
            (if (event.isAltPressed && event.metaState and KeyEvent.META_ALT_RIGHT_ON != 0) 256 else 0) or
            (if (event.isMetaPressed && event.metaState and KeyEvent.META_META_RIGHT_ON != 0) 512 else 0)

    override fun onKeyUp(code: Int, event: KeyEvent): Boolean {
        if (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN)
            return super.onKeyUp(code, event)
        // A shortcut, composition, or another surface may have consumed the down.
        val key = heldKeys.remove(event.deviceId to code) ?: return true
        return dispatchInput { surface?.key(key.code, keyModifiers(event) or key.softCtrl, "", key.unshifted, KeyAction.RELEASE) }
    }

    override fun onDraw(canvas: Canvas) {
        val checkpoint = canvas.save()
        canvas.clipRect(0, 0, width, height)
        try { drawTerminal(canvas) } finally { canvas.restoreToCount(checkpoint) }
    }
    private fun drawTerminal(canvas: Canvas) {
        val frame = currentFrame
        canvas.drawColor(frame?.background ?: (0xff000000.toInt() or style.appearance.background))
        if (frame == null) return
        currentGraphics?.draw(canvas,1,imagePaint)
        for (y in 0 until frame.rows) for (x in 0 until frame.cols) {
            val cell = frame.cells[y * frame.cols + x]
            if(cell.flags and 256==0 && cell.flags and 512!=0) continue
            fill.color = if (cell.flags and 256 != 0) Color.rgb(68, 91, 125) else cell.background
            canvas.drawRect((x * cellWidth).toFloat(), (y * cellHeight).toFloat(), ((x + 1) * cellWidth).toFloat(), ((y + 1) * cellHeight).toFloat(), fill)
        }
        currentGraphics?.draw(canvas,2,imagePaint)
        for (y in 0 until frame.rows) for (x in 0 until frame.cols) {
            val cell = frame.cells[y * frame.cols + x]
            if (cell.width == 0 || cell.flags and 8 != 0 || (cell.flags and 128 != 0 && !blink) || cell.text.takeIf { it.isNotEmpty() }?.codePointAt(0)==0x10eeee) continue
            val left = (x * cellWidth).toFloat(); val top = (y * cellHeight).toFloat()
            val right = left + cellWidth * cell.width
            canvas.save(); canvas.clipRect(left, top, right, top + cellHeight)
            paint.color = if (cell.flags and 256 != 0) Color.WHITE else cell.foreground; paint.alpha = if (cell.flags and 4 != 0) 128 else 255
            paint.isFakeBoldText = cell.flags and 1 != 0; paint.textSkewX = if (cell.flags and 2 != 0) -0.2f else 0f
            paint.isStrikeThruText = cell.flags and 16 != 0; paint.isUnderlineText = cell.flags and 32 != 0
            canvas.drawText(cell.text, left, top + baseline, paint)
            if (cell.flags and 64 != 0) canvas.drawLine(left, top + 1, right, top + 1, paint)
            canvas.restore()
        }
        currentGraphics?.draw(canvas,3,imagePaint)
        val cursorFocused = hasFocus() && hasWindowFocus()
        if (frame.cursorVisible && (!style.appearance.blink || !frame.cursorBlink || blink || !cursorFocused)) {
            val x = (frame.cursorX * cellWidth).toFloat(); val y = (frame.cursorY * cellHeight).toFloat()
            fill.color = frame.foreground; fill.alpha = if (cursorFocused) 150 else 255
            fill.style = if (cursorFocused) Paint.Style.FILL else Paint.Style.STROKE
            when (frame.cursorStyle) {
                0 -> canvas.drawRect(x, y, x + 2, y + cellHeight, fill)
                2 -> canvas.drawRect(x, y + cellHeight - 2, x + cellWidth, y + cellHeight, fill)
                else -> canvas.drawRect(x, y, x + cellWidth, y + cellHeight, fill)
            }
            fill.style = Paint.Style.FILL; fill.alpha = 255
        }
        if (selecting) selectionHandles()?.let { (start, end) ->
            fill.color = Color.rgb(120, 170, 255)
            canvas.drawCircle(start.first, start.second, handleRadius, fill)
            canvas.drawCircle(end.first, end.second, handleRadius, fill)
        }
        if (composition.isNotEmpty() && frame.cursorX >= 0) {
            val x = (frame.cursorX * cellWidth).toFloat(); val y = (frame.cursorY * cellHeight).toFloat()
            paint.color = frame.foreground; paint.alpha = 255
            paint.isUnderlineText = imeUnderlined || pendingAccents.isNotEmpty()
            paint.isStrikeThruText = false; paint.isFakeBoldText = false; paint.textSkewX = 0f
            fill.color = frame.background
            canvas.drawRect(x, y, x + paint.measureText(composition), y + cellHeight, fill)
            canvas.drawText(composition, x, y + baseline, paint)
        }
    }

    private inner class TerminalInputConnection : BaseInputConnection(this@TerminalView, true) {
        private val pending = SpannableStringBuilder()
        private var valid = true
        private var batches = 0
        private var cursorUpdates = false
        // Accepted text is no longer editable: transport may already have sent
        // it. Keep the remaining IME window at its UTF-16 document offset.
        private var origin = 0
        private var selectionDirty = false
        // A finalized window is retained only while its selection still needs
        // it. Its bytes already belong to transport and must never be replayed.
        private var finalized = false
        private fun absolute(index: Int): Int = if (index < 0) -1 else origin + index
        override fun performContextMenuAction(id: Int): Boolean {
            if (!valid) return false
            when (id) {
                android.R.id.paste -> this@TerminalView.pasteClipboard()
                android.R.id.copy -> this@TerminalView.copySelection()
                android.R.id.selectAll -> {
                    selecting = true
                    textAction { it.select(SelectionOperation.ALL); showTextActions() }
                }
                else -> return false
            }
            return true
        }
        override fun getEditable(): Editable = pending
        init { Selection.setSelection(pending, 0) }
        fun cancel() { valid = false;
            // commitText can leave plain text after its requested cursor. Keep
            // that accepted tail on focus/connection changes; active composition
            // is still explicitly cancelled, and finalized text is never resent.
            if (!finalized && getComposingSpanStart(pending) < 0 && pending.isNotEmpty())
                this@TerminalView.commitText(pending.toString())
            pending.clear(); pending.clearSpans(); imeComposition = ""; imeUnderlined = false
            pendingAccents.clear(); invalidate()
        }
        private fun changed(selectionChanged: Boolean = true) {
            imeComposition = if (finalized) "" else pending.toString()
            imeUnderlined = !finalized && getComposingSpanStart(pending) >= 0
            invalidate()
            selectionDirty = selectionDirty || selectionChanged
            if (batches == 0 && valid) {
                if (selectionDirty) {
                    inputMethod.updateSelection(this@TerminalView, absolute(Selection.getSelectionStart(pending)), absolute(Selection.getSelectionEnd(pending)),
                        absolute(getComposingSpanStart(pending)), absolute(getComposingSpanEnd(pending)))
                    selectionDirty = false
                }
                anchor()
            }
        }
        override fun beginBatchEdit(): Boolean { if (!valid || batches == Int.MAX_VALUE) return false; batches++; return true }
        override fun endBatchEdit(): Boolean { if (!valid) return false; if (batches > 0) batches--; changed(false); return batches > 0 }
        override fun setComposingText(text: CharSequence, position: Int): Boolean {
            if (!valid || text.length > Int.MAX_VALUE - origin - pending.length) return false
            if (!editableInsertion()) return false
            compactFinalized()
            val ok = super.setComposingText(text, position); changed(); return ok
        }
        override fun setComposingRegion(start: Int, end: Int): Boolean {
            if (!valid) return false
            fun local(index: Int) = (index.toLong() - origin).coerceIn(0L, pending.length.toLong()).toInt()
            val ok = super.setComposingRegion(local(start), local(end))
            // Region marking neither moves the cursor nor changes the text.
            changed(false); return ok
        }
        override fun setSelection(start: Int, end: Int): Boolean {
            if (!valid || start < origin || end < origin ||
                start.toLong() > origin.toLong() + pending.length || end.toLong() > origin.toLong() + pending.length) return false
            val ok = super.setSelection(start - origin, end - origin)
            if (ok) compactFinalized()
            changed(); return ok
        }
        override fun getTextBeforeCursor(length: Int, flags: Int): CharSequence? =
            if (!valid || length < 0) null else super.getTextBeforeCursor(length, flags)
        override fun getTextAfterCursor(length: Int, flags: Int): CharSequence? =
            if (!valid || length < 0) null else super.getTextAfterCursor(length, flags)
        override fun getSelectedText(flags: Int): CharSequence? =
            if (!valid) null else super.getSelectedText(flags)
        @android.annotation.TargetApi(31)
        override fun getSurroundingText(beforeLength: Int, afterLength: Int, flags: Int): SurroundingText? {
            if (!valid || beforeLength < 0 || afterLength < 0) return null
            val local = super.getSurroundingText(beforeLength, afterLength, flags) ?: return null
            return SurroundingText(local.text, local.selectionStart, local.selectionEnd, origin + local.offset)
        }
        override fun commitText(text: CharSequence, position: Int): Boolean {
            if (!valid || text.length > Int.MAX_VALUE - origin - pending.length) return false
            // Gboard may commit a single LF for Enter instead of sending a key
            // event. Preserve terminal Enter semantics without rewriting text blocks.
            if (text.toString() == "\n") {
                if (!acceptForReturn() || !sendKey(KeyEvent.KEYCODE_ENTER)) return false
                origin++; changed(); return true
            }
            if (!editableInsertion()) return false
            // Let Android compute the replacement and newCursorPosition on a
            // temporary editor. Rejection must leave the live buffer untouched.
            val candidate = SpannableStringBuilder(pending)
            Selection.setSelection(candidate, Selection.getSelectionStart(pending), Selection.getSelectionEnd(pending))
            val editor = object : BaseInputConnection(this@TerminalView, true) {
                override fun getEditable(): Editable = candidate
            }
            val composingStart = getComposingSpanStart(pending)
            val composingEnd = getComposingSpanEnd(pending)
            if (composingStart >= 0 && composingEnd >= 0)
                editor.setComposingRegion(composingStart, composingEnd)
            if (!editor.commitText(text, position)) return false
            if (!validInputText(candidate)) return false
            val cursor = Selection.getSelectionStart(candidate)
            val delivered = if (finalized) pending.length else 0
            if (cursor < delivered) return false
            val accepted = candidate.subSequence(delivered, cursor).toString()
            if (accepted.isNotEmpty() && !this@TerminalView.commitText(accepted)) return false
            val remaining = candidate.subSequence(cursor, candidate.length)
            origin += cursor
            pending.clear(); pending.clearSpans(); pending.append(remaining)
            finalized = false
            Selection.setSelection(pending, 0); changed(); return true
        }
        override fun finishComposingText(): Boolean {
            if (!valid) return false
            // IMEs may accept a word by ending composition rather than calling
            // commitText. Finishing preserves text; cancelling/closing discards it.
            val accepted = pending.toString()
            if (accepted.length > Int.MAX_VALUE - origin) return false
            if (!finalized && accepted.isNotEmpty() && !this@TerminalView.commitText(accepted)) return false
            finalized = true
            super.finishComposingText()
            compactFinalized(); changed(); return true
        }
        private fun compactFinalized(force: Boolean = false) {
            if (!finalized || (!force && (Selection.getSelectionStart(pending) != pending.length ||
                        Selection.getSelectionEnd(pending) != pending.length))) return
            origin += pending.length
            pending.clear(); pending.clearSpans(); Selection.setSelection(pending, 0)
            finalized = false
        }
        fun terminalKeyAccepted() {
            // External terminal navigation/editing supersedes the old read-only
            // IME selection. No uncommitted text is finalized by this operation.
            if (finalized) { compactFinalized(force = true); changed() }
        }
        private fun editableInsertion(): Boolean {
            if (!finalized) return true
            if (getComposingSpanStart(pending) < 0 && Selection.getSelectionStart(pending) == pending.length &&
                Selection.getSelectionEnd(pending) == pending.length) return true
            onError("该段文字已经发送，请使用终端按键编辑")
            return false
        }
        fun acceptForReturn(): Boolean {
            if (!valid || !finishComposingText()) return false
            // Return is a terminal action; finish itself must not move selection.
            compactFinalized(force = true); changed(); return true
        }
        override fun closeConnection() { cancel(); super.closeConnection() }
        override fun deleteSurroundingText(before: Int, after: Int): Boolean = delete(before, after, false)
        override fun deleteSurroundingTextInCodePoints(before: Int, after: Int): Boolean = delete(before, after, true)
        private fun delete(before: Int, after: Int, codepoints: Boolean): Boolean {
            if (!valid || before < 0 || after < 0 || before > 1024 || after > 1024) return false
            if (finalized) {
                if (!editableInsertion()) return false
                compactFinalized()
            }
            if (pending.isNotEmpty()) {
                // BaseInputConnection excludes the composing span when deleting surrounding
                // text. Our entire local buffer is uncommitted, so edit it at the IME cursor.
                val first = Selection.getSelectionStart(pending).coerceAtLeast(0)
                val last = Selection.getSelectionEnd(pending).coerceAtLeast(0)
                val start = minOf(first, last)
                val end = maxOf(first, last)
                val left = if (codepoints) Character.offsetByCodePoints(pending, start,
                    -before.coerceAtMost(Character.codePointCount(pending, 0, start))) else (start - before).coerceAtLeast(0)
                val right = if (codepoints) Character.offsetByCodePoints(pending, end,
                    after.coerceAtMost(Character.codePointCount(pending, end, pending.length))) else (end + after).coerceAtMost(pending.length)
                pending.delete(end, right); pending.delete(left, start)
                val removedBefore = start - left
                Selection.setSelection(pending, first - removedBefore, last - removedBefore)
                changed(); return true
            }
            repeat(before) { if (!sendKey(KeyEvent.KEYCODE_DEL)) return false }
            repeat(after) { if (!sendKey(KeyEvent.KEYCODE_FORWARD_DEL)) return false }
            return true
        }
        override fun sendKeyEvent(event: KeyEvent): Boolean {
            if (!valid) return false

            return if (event.action == KeyEvent.ACTION_DOWN) onKeyDown(event.keyCode, event)
                else if (event.action == KeyEvent.ACTION_UP) onKeyUp(event.keyCode, event) else false
        }
        override fun performEditorAction(action: Int): Boolean = valid && acceptForReturn() && sendKey(KeyEvent.KEYCODE_ENTER)
        override fun requestCursorUpdates(mode: Int): Boolean {
            if (!valid) return false
            cursorUpdates = mode and CURSOR_UPDATE_MONITOR != 0
            if (mode and CURSOR_UPDATE_IMMEDIATE != 0) anchor(true)
            return true
        }
        fun anchor(force: Boolean = false) {
            if (!valid || (!cursorUpdates && !force)) return
            val frame = currentFrame ?: return
            val location = IntArray(2); getLocationOnScreen(location)
            val transform = Matrix().apply { setTranslate(location[0].toFloat(), location[1].toFloat()) }
            val x = (frame.cursorX.coerceAtLeast(0) * cellWidth).toFloat()
            val y = (frame.cursorY.coerceAtLeast(0) * cellHeight).toFloat()
            val info = CursorAnchorInfo.Builder().setMatrix(transform)
                .setSelectionRange(absolute(Selection.getSelectionStart(pending)), absolute(Selection.getSelectionEnd(pending)))
                .setInsertionMarkerLocation(x, y, y + baseline, y + cellHeight, CursorAnchorInfo.FLAG_HAS_VISIBLE_REGION)
            val composingStart = getComposingSpanStart(pending)
            val composingEnd = getComposingSpanEnd(pending)
            if (composingStart >= 0 && composingEnd > composingStart)
                info.setComposingText(absolute(composingStart), pending.subSequence(composingStart, composingEnd))
            inputMethod.updateCursorAnchorInfo(this@TerminalView, info.build())
        }
    }
}
