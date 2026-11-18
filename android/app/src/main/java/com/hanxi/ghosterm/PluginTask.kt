package com.hanxi.ghosterm

import android.os.Looper
import com.hanxi.ghosterm.PluginViewValue.Value
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.json.JSONObject
import java.util.UUID

internal data class PluginTaskDescriptor(val viewID: String, val revision: String, val title: String, val unit: String, val cancellable: Boolean)
internal data class PluginTaskSnapshot(val viewID: String, val revision: String, val sequence: String, val phase: Phase,
    val completed: String, val total: String?, val message: String, val result: Result) {
    enum class Phase(val wire: String, val label: String) {
        QUEUED("queued", "等待开始"), RUNNING("running", "正在运行"), CANCELLING("cancelling", "等待任务确认停止"),
        SUCCEEDED("succeeded", "已完成"), FAILED("failed", "已失败"), CANCELLED("cancelled", "已确认取消"), UNKNOWN("unknown", "结果不明");
        val terminal get() = this in setOf(SUCCEEDED, FAILED, CANCELLED)
    }
    sealed interface Result {
        data object None : Result
        data class Success(val summary: String) : Result
        data class Failed(val code: String, val message: String) : Result
        data class Cancelled(val effect: String) : Result
        data class Unknown(val reason: String) : Result
        val text: String? get() = when (this) {
            None -> null
            is Success -> summary
            is Failed -> "$code：$message"
            is Cancelled -> if (effect == "none") "任务已停止，无外部影响" else "任务已停止，可能已有外部影响"
            is Unknown -> reason
        }
    }
}

internal object PluginTaskCodec {
    class RPCFailure(val kind: String, val code: String?, val detail: String?, val effect: String) : Exception() {
        override val message get() = if (kind == "failed") "请求失败（$code）：$detail；${if (effect == "none") "未产生执行效果" else "可能已有执行效果"}"
            else "请求已取消；${if (effect == "none") "未产生执行效果" else "可能已有执行效果"}，任务状态需另行读取"
    }
    private fun success(bytes: ByteArray) = with(PluginViewValue) {
        val envelope = envelope(bytes, 8192)
        when ((envelope["kind"] as? Value.Text)?.value) {
            "failed" -> {
                envelope.keys(setOf("kind", "error")); val error = envelope["error"].fields(); error.keys(setOf("code", "message", "effect"))
                val code = error["code"].text(64, true)
                require(code in setOf("UNSUPPORTED", "DENIED", "EXPIRED", "INVALID_INPUT", "RESOURCE_LIMIT", "CANCELLED", "IO", "PROTOCOL", "INTERNAL", "BUSY", "INTEGRITY"))
                val message = (error["message"] as? Value.Text)?.value ?: error("错误消息必须是字符串")
                require(message.codePointCount(0, message.length) in 1..1024)
                val visible = message.map { if (it.code < 32 || it.code == 127) "\\u" + it.code.toString(16).padStart(4, '0') else it.toString() }.joinToString("")
                throw RPCFailure("failed", code, visible, effect(error["effect"]))
            }
            "cancelled" -> { envelope.keys(setOf("kind", "effect")); throw RPCFailure("cancelled", null, null, effect(envelope["effect"])) }
            else -> PluginViewValue.success(bytes)
        }
    }
    private fun effect(value: PluginViewValue.Value?) = with(PluginViewValue) { value.text(8, true).also { require(it in setOf("none", "possible")) } }
    private fun counter(value: PluginViewValue.Value?) = with(PluginViewValue) {
        if ((value as? Value.Text)?.value == "0") "0" else value.revision()
    }
    private fun scope(value: Map<String, PluginViewValue.Value>, description: PluginTaskDescriptor) = with(PluginViewValue) {
        require(value["viewId"].id() == description.viewID && value["revision"].revision() == description.revision) { "任务身份或修订已变化" }
    }
    fun descriptor(bytes: ByteArray) = with(PluginViewValue) {
        val value = PluginTaskCodec.success(bytes); value.keys(setOf("viewId", "revision", "title", "unit", "cancellable"))
        val flag = (value["cancellable"] as? Value.Bool)?.value ?: error("取消能力必须是布尔值")
        PluginTaskDescriptor(value["viewId"].id(), value["revision"].revision(), value["title"].text(128, true), value["unit"].text(64, true), flag)
    }
    fun snapshot(bytes: ByteArray, description: PluginTaskDescriptor, previous: PluginTaskSnapshot? = null) = with(PluginViewValue) {
        val value = PluginTaskCodec.success(bytes); value.keys(setOf("viewId", "revision", "sequence", "phase", "completed", "total", "message", "result")); scope(value, description)
        val phase = PluginTaskSnapshot.Phase.entries.firstOrNull { it.wire == value["phase"].text(16, true) } ?: error("任务阶段无效")
        val total = value["total"].fields()
        val units = if ((total["kind"] as? Value.Text)?.value == "known") { total.keys(setOf("kind", "units")); counter(total["units"]) }
            else { total.keys(setOf("kind")); require(total["kind"].text(8) == "unknown"); null }
        val completed = counter(value["completed"])
        require(units == null || completed.toULong() <= units.toULong()) { "任务进度超过总量" }
        require(phase != PluginTaskSnapshot.Phase.QUEUED || completed == "0")
        require(description.cancellable || phase !in setOf(PluginTaskSnapshot.Phase.CANCELLING, PluginTaskSnapshot.Phase.CANCELLED))
        val result = value["result"].fields()
        val parsed = when (phase) {
            PluginTaskSnapshot.Phase.SUCCEEDED -> {
                result.keys(setOf("kind", "summary")); require(result["kind"].text(8) == "success" && (units == null || completed == units))
                PluginTaskSnapshot.Result.Success(result["summary"].text(256, true))
            }
            PluginTaskSnapshot.Phase.FAILED -> {
                result.keys(setOf("kind", "code", "message")); require(result["kind"].text(8) == "failed")
                PluginTaskSnapshot.Result.Failed(result["code"].text(64, true), result["message"].text(256, true))
            }
            PluginTaskSnapshot.Phase.CANCELLED -> {
                result.keys(setOf("kind", "effect")); require(result["kind"].text(16) == "cancelled"); PluginTaskSnapshot.Result.Cancelled(effect(result["effect"]))
            }
            PluginTaskSnapshot.Phase.UNKNOWN -> {
                result.keys(setOf("kind", "reason")); require(result["kind"].text(8) == "unknown"); PluginTaskSnapshot.Result.Unknown(result["reason"].text(128, true))
            }
            else -> { result.keys(setOf("kind")); require(result["kind"].text(8) == "none"); PluginTaskSnapshot.Result.None }
        }
        val p = PluginTaskSnapshot(description.viewID, description.revision, value["sequence"].revision(), phase, completed, units, value["message"].text(128), parsed)
        if (previous != null) {
            require(previous.viewID == description.viewID && previous.revision == description.revision)
            require(p.sequence.toULong() >= previous.sequence.toULong() && (p.sequence != previous.sequence || p == previous)) { "任务序号倒退或改写" }
            require(p.completed.toULong() >= previous.completed.toULong()) { "任务进度倒退" }
            require(previous.total == null || p.total == previous.total) { "已知总量不可变化" }
            require(!previous.phase.terminal || p == previous) { "已确认终态不可改写" }
            require(previous.phase != PluginTaskSnapshot.Phase.RUNNING || p.phase != PluginTaskSnapshot.Phase.QUEUED)
            require(previous.phase != PluginTaskSnapshot.Phase.CANCELLING || p.phase !in setOf(PluginTaskSnapshot.Phase.QUEUED, PluginTaskSnapshot.Phase.RUNNING))
        }
        p
    }
    fun cancellation(bytes: ByteArray, description: PluginTaskDescriptor, requestID: String): String = with(PluginViewValue) {
        val value = PluginTaskCodec.success(bytes); value.keys(setOf("viewId", "revision", "requestId", "kind")); scope(value, description)
        require(description.cancellable && value["requestId"].id() == requestID) { "取消回执身份不匹配" }
        value["kind"].text(16).also { require(it in setOf("requested", "alreadyFinished")) }
    }
}

internal class PluginTaskModel(private val scope: CoroutineScope, private val request: suspend (String, ByteArray) -> ByteArray,
    private val expectedViewID: String? = null, private val history: PluginTaskHistory? = null,
    private val isCurrent: () -> Boolean = { true }) {
    companion object { val methods = listOf("view.task.cancel", "view.task.describe", "view.task.read") }
    enum class Cancellation { IDLE, PENDING, REQUESTED, ALREADY_FINISHED, UNCONFIRMED, FAILED, CANCELLED }
    data class State(val descriptor: PluginTaskDescriptor? = null, val snapshot: PluginTaskSnapshot? = null,
        val busy: Boolean = false, val valid: Boolean = true, val issue: String? = null,
        val cancellation: Cancellation = Cancellation.IDLE, val cancelRequestID: String? = null)
    private val mutable = MutableStateFlow(State()); val state = mutable.asStateFlow()
    private var generation = Any()
    private var readJob: Job? = null
    val available get() = state.value.valid && scope.isActive && isCurrent()
    val canCancel get() = available && state.value.descriptor?.cancellable == true && state.value.cancelRequestID == null &&
        state.value.snapshot?.phase in setOf(PluginTaskSnapshot.Phase.QUEUED, PluginTaskSnapshot.Phase.RUNNING)
    val canAutoRefresh get() = available && !state.value.busy && state.value.issue == null && state.value.cancellation != Cancellation.UNCONFIRMED &&
        state.value.snapshot?.let { !it.phase.terminal && it.phase != PluginTaskSnapshot.Phase.UNKNOWN } == true
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) { "Task requires Main owner" } }
    fun load(): Job? {
        main(); if (!available || state.value.busy) return null
        val expected = Any(); generation = expected; mutable.value = state.value.copy(busy = true, issue = null)
        val job = scope.launch(start = CoroutineStart.UNDISPATCHED) {
            try {
                var description = state.value.descriptor
                if (description == null) {
                    val bytes = request("view.task.describe", "{}".toByteArray())
                    if (!current(expected)) return@launch
                    description = PluginTaskCodec.descriptor(bytes)
                    require(expectedViewID == null || expectedViewID == description.viewID) { "视图描述符 ID 与注册不一致" }
                    val prior = history?.restore(description)
                    mutable.value = if (prior == null) state.value.copy(descriptor = description) else state.value.copy(descriptor = description,
                        snapshot = prior.snapshot, cancelRequestID = prior.requestID,
                        cancellation = if (prior.cancellation == Cancellation.PENDING) Cancellation.UNCONFIRMED else prior.cancellation)
                }
                read(description, expected)
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: PluginTaskCodec.RPCFailure) { if (current(expected)) mutable.value = state.value.copy(issue = "${failure.message}。保留上次观察。") }
            catch (failure: Exception) { if (current(expected)) mutable.value = state.value.copy(issue = "读取结果不明，保留上次观察。可显式重新读取：${failure.message}") }
            finally { if (generation === expected) mutable.value = state.value.copy(busy = false) }
        }
        readJob = job
        return job
    }
    fun cancel(): Job? {
        main(); val description = state.value.descriptor ?: return null; if (!canCancel) return null
        val expected = Any(); generation = expected; val requestID = UUID.randomUUID().toString()
        mutable.value = state.value.copy(busy = true, issue = null, cancelRequestID = requestID, cancellation = Cancellation.PENDING)
        readJob?.cancel()
        readJob = null
        history?.action(description, requestID, state.value.cancellation)
        return scope.launch(start = CoroutineStart.UNDISPATCHED) {
            try {
                val bytes = request("view.task.cancel", input(description).put("requestId", requestID).toString().toByteArray(Charsets.UTF_8))
                if (!current(expected)) return@launch
                val kind = PluginTaskCodec.cancellation(bytes, description, requestID)
                mutable.value = state.value.copy(cancellation = if (kind == "requested") Cancellation.REQUESTED else Cancellation.ALREADY_FINISHED)
                try { read(description, expected) }
                catch (cancelled: CancellationException) { throw cancelled }
                catch (failure: Exception) { if (current(expected)) mutable.value = state.value.copy(issue = "取消回执已收到；读取任务状态失败或不明：${failure.message}") }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: PluginTaskCodec.RPCFailure) {
                if (current(expected)) mutable.value = state.value.copy(cancellation = if (failure.kind == "failed") Cancellation.FAILED else Cancellation.CANCELLED,
                    issue = "取消${failure.message}。不会自动再次发送。")
            } catch (failure: Exception) {
                if (current(expected)) mutable.value = state.value.copy(cancellation = Cancellation.UNCONFIRMED, issue = "取消结果不明，不会再次发送。请显式读取任务状态。")
            } finally {
                if (generation === expected) {
                    mutable.value = if (state.value.cancellation == Cancellation.PENDING) state.value.copy(busy = false, cancellation = Cancellation.UNCONFIRMED,
                        issue = "取消结果不明，不会再次发送。请显式读取任务状态。") else state.value.copy(busy = false)
                }
                history?.action(description, requestID, state.value.cancellation)
            }
        }
    }
    fun invalidate() {
        main(); generation = Any()
        mutable.value = state.value.copy(valid = false, busy = false,
            cancellation = if (state.value.cancellation == Cancellation.PENDING) Cancellation.UNCONFIRMED else state.value.cancellation,
            issue = if (state.value.snapshot?.phase?.terminal == true) "任务所属会话已结束；保留此前已确认的结果" else "任务所属会话已结束；保留最后观察，当前运行状态不明")
        val last = state.value
        if (last.descriptor != null && last.cancelRequestID != null) history?.action(last.descriptor, last.cancelRequestID, last.cancellation)
    }
    private suspend fun current(expected: Any) = generation === expected && available && currentCoroutineContext().isActive
    private fun input(d: PluginTaskDescriptor) = JSONObject().put("viewId", d.viewID).put("revision", d.revision)
    private suspend fun read(description: PluginTaskDescriptor, expected: Any) {
        val bytes = request("view.task.read", input(description).toString().toByteArray(Charsets.UTF_8))
        if (current(expected)) {
            val snapshot = PluginTaskCodec.snapshot(bytes, description, state.value.snapshot)
            mutable.value = state.value.copy(snapshot = snapshot); history?.observe(description, snapshot)
        }
    }
}
