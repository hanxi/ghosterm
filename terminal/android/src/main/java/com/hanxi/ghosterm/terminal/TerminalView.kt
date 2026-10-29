package com.hanxi.ghosterm.terminal

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.Typeface
import android.os.SystemClock
import android.text.Editable
import android.text.InputType
import android.text.Selection
import android.text.SpannableStringBuilder
import android.view.GestureDetector
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.CursorAnchorInfo
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import kotlin.math.ceil

/** Canvas consumes cell snapshots; neither PTY output nor transport types enter this View. */
class TerminalView(context: Context) : View(context) {
    var surface: TerminalSurface? = null
        set(value) {
            if (field === value) return
            connection?.cancel(); connection = null
            field = value; currentFrame = null; lastSize = null
            ctrl = 0; updateSize(); invalidate()
            inputMethod.restartInput(this)
        }
    var onError: (String) -> Unit = {}
    var onModifiersChanged: (Int) -> Unit = {}
    /** 0 off, 1 one-shot, 2 locked. */
    var ctrl: Int = 0
        set(value) { field = value; onModifiersChanged(value) }
    var currentFrame: TerminalFrame? = null
        private set
    var composition: String = ""
        private set
    private var connection: TerminalInputConnection? = null
    private val inputMethod get() = context.getSystemService(InputMethodManager::class.java)
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Typeface.MONOSPACE }
    private val fill = Paint()
    private var cellWidth = 1
    private var cellHeight = 1
    private var baseline = 1f
    private var lastSize: List<Int>? = null
    private var running = false
    private var blink = true
    private var fractionalScroll = 0f
    private val gestures = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDown(event: MotionEvent) = true
        override fun onSingleTapUp(event: MotionEvent): Boolean { performClick(); return true }
        override fun onScroll(first: MotionEvent?, last: MotionEvent, dx: Float, dy: Float): Boolean {
            fractionalScroll += dy
            val lines = (fractionalScroll / cellHeight).toInt()
            if (lines != 0) { dispatch { surface?.scroll(lines) }; fractionalScroll -= lines * cellHeight }
            return true
        }
    })
    private val tick = object : Runnable {
        override fun run() {
            if (!running) return
            dispatch {
                surface?.frame(currentFrame?.revision ?: 0)?.let {
                    currentFrame = it; invalidate(); connection?.anchor()
                }
            }
            val next = SystemClock.uptimeMillis() % 1000 < 500
            if (next != blink) { blink = next; if (currentFrame?.cursorBlink == true) invalidate() }
            postDelayed(this, 32)
        }
    }
    init {
        isFocusable = true; isFocusableInTouchMode = true
        contentDescription = "终端"
        importantForAutofill = IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS
        setFontSize(14f)
    }
    fun setFontSize(sp: Float) {
        require(sp in 8f..32f)
        paint.textSize = sp * resources.displayMetrics.scaledDensity
        cellWidth = ceil(paint.measureText("M")).toInt().coerceAtLeast(1)
        cellHeight = ceil(paint.fontMetrics.descent - paint.fontMetrics.ascent).toInt().coerceAtLeast(1)
        baseline = -paint.fontMetrics.ascent
        lastSize = null; updateSize(); invalidate()
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
    private fun rendering() {
        val next = isAttachedToWindow && isShown && windowVisibility == VISIBLE
        if (next == running) return
        running = next
        if (running) post(tick) else removeCallbacks(tick)
    }
    override fun onAttachedToWindow() {
        super.onAttachedToWindow(); rendering()
        // AndroidView is created before it joins the focus tree. Request once
        // attached, without showing the IME or stealing focus from a later page.
        post { if (isAttachedToWindow && isShown) requestFocus() }
    }
    override fun onDetachedFromWindow() {
        running = false; removeCallbacks(tick); connection?.cancel(); connection = null
        super.onDetachedFromWindow()
    }
    override fun onVisibilityAggregated(visible: Boolean) { super.onVisibilityAggregated(visible); rendering() }
    override fun onWindowVisibilityChanged(visibility: Int) { super.onWindowVisibilityChanged(visibility); rendering() }
    override fun onFocusChanged(gain: Boolean, direction: Int, previous: Rect?) {
        super.onFocusChanged(gain, direction, previous)
        if (!gain) { connection?.cancel(); connection = null; ctrl = 0 }
        invalidate()
    }
    override fun performClick(): Boolean {
        super.performClick(); requestFocus(); inputMethod.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT); return true
    }
    override fun onTouchEvent(event: MotionEvent): Boolean = gestures.onTouchEvent(event)
    fun showKeyboard() { requestFocus(); inputMethod.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT) }
    fun hideKeyboard() { inputMethod.hideSoftInputFromWindow(windowToken, 0) }
    fun sendKey(code: Int, modifiers: Int = 0, text: String = ""): Boolean {
        val sent = dispatch { surface?.key(code, modifiers or if (ctrl != 0) 2 else 0, text) }
        if (sent && ctrl == 1) ctrl = 0
        return sent
    }
    fun commitText(text: String): Boolean {
        if (ctrl != 0 && text.length == 1 && text[0].lowercaseChar() in 'a'..'z')
            return sendKey(KeyEvent.KEYCODE_A + text[0].lowercaseChar().code - 'a'.code, text = text)
        val sent = dispatch { surface?.commit(text) }
        if (sent && ctrl == 1) ctrl = 0
        return sent
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
        if (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_VOLUME_DOWN || code == KeyEvent.KEYCODE_VOLUME_UP)
            return super.onKeyDown(code, event)
        if (KeyEvent.isModifierKey(code)) return true
        val unicode = event.getUnicodeChar(event.metaState and KeyEvent.META_CTRL_MASK.inv() and KeyEvent.META_META_MASK.inv())
        if (unicode and android.view.KeyCharacterMap.COMBINING_ACCENT != 0) return super.onKeyDown(code, event)
        val text = if (unicode > 0) String(Character.toChars(unicode)) else ""
        val modifiers = (if (event.isShiftPressed) 1 else 0) or (if (event.isCtrlPressed) 2 else 0) or
            (if (event.isAltPressed) 4 else 0) or (if (event.isMetaPressed) 8 else 0)
        return sendKey(code, modifiers, text)
    }
    override fun onKeyUp(code: Int, event: KeyEvent): Boolean = if (code == KeyEvent.KEYCODE_BACK ||
        code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN) super.onKeyUp(code, event) else true

    override fun onDraw(canvas: Canvas) {
        val checkpoint = canvas.save()
        canvas.clipRect(0, 0, width, height)
        try { drawTerminal(canvas) } finally { canvas.restoreToCount(checkpoint) }
    }
    private fun drawTerminal(canvas: Canvas) {
        val frame = currentFrame
        canvas.drawColor(frame?.background ?: Color.rgb(23, 27, 32))
        if (frame == null) return
        for (y in 0 until frame.rows) for (x in 0 until frame.cols) {
            val cell = frame.cells[y * frame.cols + x]
            fill.color = cell.background
            canvas.drawRect((x * cellWidth).toFloat(), (y * cellHeight).toFloat(), ((x + 1) * cellWidth).toFloat(), ((y + 1) * cellHeight).toFloat(), fill)
        }
        for (y in 0 until frame.rows) for (x in 0 until frame.cols) {
            val cell = frame.cells[y * frame.cols + x]
            if (cell.width == 0 || cell.flags and 8 != 0 || (cell.flags and 128 != 0 && !blink)) continue
            val left = (x * cellWidth).toFloat(); val top = (y * cellHeight).toFloat()
            val right = left + cellWidth * cell.width
            canvas.save(); canvas.clipRect(left, top, right, top + cellHeight)
            paint.color = cell.foreground; paint.alpha = if (cell.flags and 4 != 0) 128 else 255
            paint.isFakeBoldText = cell.flags and 1 != 0; paint.textSkewX = if (cell.flags and 2 != 0) -0.2f else 0f
            paint.isStrikeThruText = cell.flags and 16 != 0; paint.isUnderlineText = cell.flags and 32 != 0
            canvas.drawText(cell.text, left, top + baseline, paint)
            if (cell.flags and 64 != 0) canvas.drawLine(left, top + 1, right, top + 1, paint)
            canvas.restore()
        }
        if (frame.cursorVisible && (!frame.cursorBlink || blink || !hasFocus())) {
            val x = (frame.cursorX * cellWidth).toFloat(); val y = (frame.cursorY * cellHeight).toFloat()
            fill.color = frame.foreground; fill.alpha = if (hasFocus()) 150 else 255
            fill.style = if (hasFocus()) Paint.Style.FILL else Paint.Style.STROKE
            when (frame.cursorStyle) {
                0 -> canvas.drawRect(x, y, x + 2, y + cellHeight, fill)
                2 -> canvas.drawRect(x, y + cellHeight - 2, x + cellWidth, y + cellHeight, fill)
                else -> canvas.drawRect(x, y, x + cellWidth, y + cellHeight, fill)
            }
            fill.style = Paint.Style.FILL; fill.alpha = 255
        }
        if (composition.isNotEmpty() && frame.cursorX >= 0) {
            val x = (frame.cursorX * cellWidth).toFloat(); val y = (frame.cursorY * cellHeight).toFloat()
            paint.color = frame.foreground; paint.alpha = 255; paint.isUnderlineText = true
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
        override fun getEditable(): Editable = pending
        init { Selection.setSelection(pending, 0) }
        fun cancel() { valid = false; pending.clear(); composition = ""; invalidate() }
        private fun changed() {
            composition = pending.toString(); invalidate()
            if (batches == 0 && valid) {
                inputMethod.updateSelection(this@TerminalView, Selection.getSelectionStart(pending), Selection.getSelectionEnd(pending),
                    getComposingSpanStart(pending), getComposingSpanEnd(pending))
                anchor()
            }
        }
        override fun beginBatchEdit(): Boolean { if (!valid) return false; batches++; return true }
        override fun endBatchEdit(): Boolean { if (!valid) return false; if (batches > 0) batches--; changed(); return batches > 0 }
        override fun setComposingText(text: CharSequence, position: Int): Boolean {
            if (!valid) return false
            val ok = super.setComposingText(text, position); changed(); return ok
        }
        override fun setComposingRegion(start: Int, end: Int): Boolean {
            if (!valid) return false
            val ok = super.setComposingRegion(start, end); changed(); return ok
        }
        override fun commitText(text: CharSequence, position: Int): Boolean {
            if (!valid || !this@TerminalView.commitText(text.toString())) return false
            pending.clear(); Selection.setSelection(pending, 0); changed(); return true
        }
        override fun finishComposingText(): Boolean {
            if (!valid) return false
            super.finishComposingText(); pending.clear(); Selection.setSelection(pending, 0); changed(); return true
        }
        override fun closeConnection() { cancel(); super.closeConnection() }
        override fun deleteSurroundingText(before: Int, after: Int): Boolean = delete(before, after, false)
        override fun deleteSurroundingTextInCodePoints(before: Int, after: Int): Boolean = delete(before, after, true)
        private fun delete(before: Int, after: Int, codepoints: Boolean): Boolean {
            if (!valid || before < 0 || after < 0 || before > 1024 || after > 1024) return false
            if (pending.isNotEmpty()) {
                // BaseInputConnection excludes the composing span when deleting surrounding
                // text. Our entire local buffer is uncommitted, so edit it at the IME cursor.
                val start = Selection.getSelectionStart(pending).coerceAtLeast(0)
                val end = Selection.getSelectionEnd(pending).coerceAtLeast(start)
                val left = if (codepoints) Character.offsetByCodePoints(pending, start,
                    -before.coerceAtMost(Character.codePointCount(pending, 0, start))) else (start - before).coerceAtLeast(0)
                val right = if (codepoints) Character.offsetByCodePoints(pending, end,
                    after.coerceAtMost(Character.codePointCount(pending, end, pending.length))) else (end + after).coerceAtMost(pending.length)
                pending.delete(end, right); pending.delete(left, start)
                Selection.setSelection(pending, left)
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
        override fun performEditorAction(action: Int): Boolean = valid && sendKey(KeyEvent.KEYCODE_ENTER)
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
                .setSelectionRange(Selection.getSelectionStart(pending), Selection.getSelectionEnd(pending))
                .setInsertionMarkerLocation(x, y, y + baseline, y + cellHeight, CursorAnchorInfo.FLAG_HAS_VISIBLE_REGION)
            if (pending.isNotEmpty()) info.setComposingText(0, pending)
            inputMethod.updateCursorAnchorInfo(this@TerminalView, info.build())
        }
    }
}
