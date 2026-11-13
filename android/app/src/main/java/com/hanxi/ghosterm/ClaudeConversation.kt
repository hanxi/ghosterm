package com.hanxi.ghosterm

import android.os.Looper
import android.util.Base64
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale

/** Per-Runtime Main owner. Actions and poll share admission; decisions are
 * rechecked after waiting, and uncertain outcomes never authorize a retry. */
internal class ClaudeConversation(private val scope: CoroutineScope,
    private val request: suspend (String, ByteArray) -> ByteArray,
    private val isCurrent: () -> Boolean = { true },
    private val observe: (suspend () -> ClaudeRPCClient.Observation)? = null) {
    enum class Phase { NEW, STARTING, READY, RUNNING, INTERRUPTING, CLOSED, FAILED, LOST, UNKNOWN }
    data class Reference(val scope: String, val requestID: String, val token: String) {
        fun json() = JSONObject().put("scope", scope).put("requestID", requestID).put("token", token)
        val id get() = Base64.encodeToString(ClaudeJson.utf8(ClaudeJson.compact(json())), Base64.NO_WRAP)
    }
    data class Option(val label: String, val description: String)
    data class Question(val question: String, val header: String, val multiSelect: Boolean, val options: List<Option>)
    data class Prompt(val reference: Reference, val toolName: String, val turnID: String, val inputJSON: String,
        val metadataJSON: String, val questions: List<Question>) { val isQuestion get() = toolName == "AskUserQuestion" }
    data class Entry(val id: String, val title: String, val text: String, val byteCount: Int)
    data class Record(val id: Int, val json: String, val byteCount: Int)
    data class State(val phase: Phase = Phase.NEW, val busy: Boolean = false, val draft: String = "",
        val activeMessageID: String? = null, val pending: List<Prompt> = emptyList(), val entries: List<Entry> = emptyList(),
        val records: List<Record> = emptyList(), val omittedEntries: Int = 0, val omittedRecords: Int = 0, val issue: String? = null,
        val observation: ClaudeRPCClient.Observation? = null, val observationIssue: String? = null)
    private val mutable = MutableStateFlow(State())
    val state = mutable.asStateFlow()
    private var current: State
        get() = mutable.value
        set(value) { mutable.value = value }
    private var detached = false
    private var valid = true
    private var generation = Any()
    private var transferring = false
    private var polling: Job? = null
    private var sessionSequence = 0L
    private var recordSequence = 0
    private val finishedInputs = mutableListOf<String>()
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) { "Claude conversation requires Main owner" } }
    val canObserve get() = !detached && isCurrent() && current.phase == Phase.UNKNOWN && !current.busy && observe != null
    fun observeOutcome() {
        main(); val observer = observe ?: return
        if (!canObserve) return
        val expected = generation; current = current.copy(busy = true, observationIssue = null)
        scope.launch {
            try {
                val result = observer()
                main(); currentCoroutineContext().ensureActive()
                check(!detached && generation === expected && isCurrent()) { "Claude observation owner changed" }
                current = current.copy(observation = result)
            } catch (failure: Exception) {
                if (!detached && generation === expected && isCurrent()) current = current.copy(
                    observationIssue = "回包观察未能确认：${failure.message ?: failure.javaClass.simpleName}")
            } finally { if (generation === expected) current = current.copy(busy = false) }
        }
    }
    val canStart get() = valid && isCurrent() && current.phase == Phase.NEW && !current.busy
    val canSend get() = valid && isCurrent() && current.phase == Phase.READY && current.activeMessageID == null && !current.busy
    val canInterrupt get() = valid && isCurrent() && current.phase == Phase.RUNNING && current.activeMessageID != null && !current.busy
    val canStop get() = valid && isCurrent() && current.phase in setOf(Phase.STARTING, Phase.READY, Phase.RUNNING, Phase.INTERRUPTING) && !current.busy
    fun canRespond(reference: Reference) = valid && isCurrent() && !current.busy && current.phase == Phase.RUNNING &&
        current.pending.any { it.reference == reference }
    fun editDraft(text: String) { main(); current = current.copy(draft = text) }

    fun start() {
        main(); if (!canStart) return
        action {
            requireAction(current.phase == Phase.NEW)
            val reply = call("start")
            require(reply.opt("requested") == true && reply.opt("sdkVersion") == "0.3.323" && reply.opt("cliVersion") == "2.1.323")
            current = current.copy(phase = Phase.STARTING)
            beginPolling()
        }
    }
    fun send() {
        main(); if (!canSend || current.draft.isBlank()) return
        val text = current.draft
        action {
            requireAction(current.phase == Phase.READY && current.activeMessageID == null)
            val reply = call("send", JSONObject().put("text", text))
            val id = identity(reply.opt("messageID"))
            current = current.copy(activeMessageID = id, phase = Phase.RUNNING, draft = if (current.draft == text) "" else current.draft)
            append("input:$id", "你 · 已提交", text)
        }
    }
    fun interrupt() {
        main(); if (!canInterrupt) return
        val id = current.activeMessageID
        action {
            requireAction(current.phase == Phase.RUNNING && current.activeMessageID == id)
            require(call("interrupt").opt("requested") == true)
            current = current.copy(phase = Phase.INTERRUPTING, pending = emptyList())
        }
    }
    fun respond(reference: Reference, allow: Boolean) {
        main()
        val prompt = current.pending.firstOrNull { it.reference == reference } ?: return
        if (!canRespond(reference) || allow && prompt.isQuestion) return
        action {
            checkPrompt(reference)
            val reply = call("approve", JSONObject().put("reference", reference.json()).put("allow", allow))
            accepted(reply, reference)
        }
    }
    fun answer(reference: Reference, answers: List<String>) {
        main()
        val prompt = current.pending.firstOrNull { it.reference == reference } ?: return
        if (!canRespond(reference) || !prompt.isQuestion || answers.size != prompt.questions.size ||
            answers.any { it.isEmpty() || ClaudeJson.utf8(it).size > 2048 }) return
        val values = JSONObject(); prompt.questions.zip(answers).forEach { (question, answer) -> values.put(question.question, answer) }
        action {
            checkPrompt(reference)
            accepted(call("answer", JSONObject().put("reference", reference.json()).put("answers", values)), reference)
        }
    }
    fun stop() {
        main(); if (!canStop) return
        action {
            val reply = call("stop"); val next = phase(reply.opt("phase"))
            require(next in setOf(Phase.CLOSED, Phase.FAILED, Phase.LOST))
            current = current.copy(phase = next, pending = emptyList(), issue = if (current.activeMessageID != null)
                "会话已停止；最后一条输入的结果尚未确认。" else current.issue)
            valid = false; polling?.cancel(); polling = null
        }
    }
    fun invalidate() {
        main(); generation = Any(); detached = true; valid = false; polling?.cancel(); polling = null
        current = current.copy(busy = false, pending = emptyList())
        if (current.phase !in setOf(Phase.CLOSED, Phase.FAILED, Phase.LOST)) current = current.copy(
            phase = Phase.UNKNOWN, issue = "连接观察已结束；未确认的结果保持未知。")
    }

    private class InvalidAction : Exception()
    private class Rejected : Exception()
    private class TooLarge : Exception()
    private fun requireAction(value: Boolean) { if (!value) throw InvalidAction() }
    private suspend fun checkCurrent(expected: Any) {
        main(); currentCoroutineContext().ensureActive()
        if (!valid || generation !== expected || !isCurrent()) throw CancellationException("Claude Runtime changed")
    }
    private fun checkPrompt(reference: Reference) {
        val prompt = current.pending.firstOrNull { it.reference == reference }
        requireAction(current.phase == Phase.RUNNING && prompt != null && prompt.turnID == current.activeMessageID)
    }
    private fun accepted(reply: JSONObject, reference: Reference) {
        val accepted = reply.opt("accepted") as? Boolean ?: error("Missing Claude acceptance")
        current = current.copy(pending = current.pending.filterNot { it.reference == reference },
            issue = if (!accepted) "此请求已失效，后端未接受决定。" else current.issue)
    }
    private fun action(block: suspend () -> Unit) {
        if (!valid || !isCurrent() || current.busy) return
        val expected = generation
        current = current.copy(busy = true, issue = null)
        scope.launch {
            try {
                while (transferring) { checkCurrent(expected); delay(5) }
                checkCurrent(expected); transferring = true
                try { block() } finally { transferring = false }
            } catch (_: Rejected) { if (generation === expected) current = current.copy(issue = "后端未接受此操作；请核对当前会话状态。") }
            catch (_: InvalidAction) { if (generation === expected) current = current.copy(issue = "当前输入或问题已改变，未发送此操作。") }
            catch (_: TooLarge) { if (generation === expected) current = current.copy(issue = "参数超过 64,000 字节上限，未发送此操作。") }
            catch (failure: Exception) {
                if (generation === expected && valid) fail("操作未能确认：${failure.message ?: failure.javaClass.simpleName}")
            } finally { if (generation === expected) current = current.copy(busy = false) }
        }
    }
    private suspend fun call(method: String, parameters: JSONObject = JSONObject()): JSONObject {
        val expected = generation; checkCurrent(expected)
        val bytes = ClaudeJson.utf8(ClaudeJson.compact(parameters))
        if (bytes.size > 64000) throw TooLarge()
        val raw = request(method, bytes); checkCurrent(expected)
        val response = ClaudeJson.objectValue(raw)
        val ok = response.opt("ok") as? Boolean ?: error("Missing Claude action status")
        if (!ok) { require(response.opt("error") == "ACTION_REJECTED"); throw Rejected() }
        val result = response.opt("result") as? JSONObject ?: error("Missing Claude action result")
        if (method == "poll" && result.opt("source") != "none" &&
            (result.opt("source") != "batch" || result.optJSONArray("events")?.length() != 0)) retain(raw)
        return result
    }
    private fun beginPolling() {
        if (polling != null) return
        polling = scope.launch {
            while (valid) { pollOnce(); delay(100) }
        }
    }
    internal suspend fun pollOnce() {
        main(); if (!valid || current.busy || transferring) return
        val expected = generation; transferring = true
        try {
            val result = call("poll", JSONObject().put("maxEvents", 32))
            if (result.opt("source") == "batch") {
                val events = result.optJSONArray("events") ?: error("Missing Claude batch")
                require(events.length() <= 32)
                if (events.length() == 0) consume(JSONObject().put("source", "none").put("phase", result.opt("phase")))
                for (index in 0 until events.length()) { if (!valid) break; consume(events.getJSONObject(index)) }
            } else consume(result)
        } catch (failure: Exception) {
            if (generation === expected && valid) fail("后端观察中断：${failure.message ?: failure.javaClass.simpleName}")
        } finally { transferring = false }
    }
    private fun fail(message: String, phase: Phase = Phase.UNKNOWN) {
        current = current.copy(phase = phase, issue = message, pending = emptyList()); valid = false
        polling?.cancel(); polling = null
    }
    private fun retain(bytes: ByteArray) {
        val records = (current.records + Record(++recordSequence, bytes.toString(Charsets.UTF_8), bytes.size)).toMutableList()
        var omitted = current.omittedRecords
        while (records.size > 128 || records.sumOf { it.byteCount } > 2097152) { records.removeAt(0); omitted++ }
        current = current.copy(records = records, omittedRecords = omitted)
    }
    private fun append(id: String, title: String, text: String) {
        val bytes = ClaudeJson.utf8(text).size
        require(bytes <= 600000) { "单条消息超出本机保留上限；当前输入结果保持未知。" }
        val entries = (current.entries.filterNot { it.id == id } + Entry(id, title, text, bytes)).toMutableList()
        var omitted = current.omittedEntries
        while (entries.size > 128 || entries.sumOf { it.byteCount } > 2097152) { entries.removeAt(0); omitted++ }
        current = current.copy(entries = entries, omittedEntries = omitted)
    }
    private fun consume(result: JSONObject) {
        when (result.opt("source")) {
            "none" -> {
                val next = phase(result.opt("phase")); require(next != Phase.NEW)
                require(next != Phase.READY || current.activeMessageID == null)
                current = current.copy(phase = next)
                if (next in setOf(Phase.CLOSED, Phase.FAILED, Phase.LOST)) ended(next)
            }
            "approval" -> {
                val event = result.getJSONObject("event")
                when (event.opt("kind")) {
                    "requested" -> {
                        val prompt = prompt(event.getJSONObject("request"))
                        if (prompt.turnID in finishedInputs || current.phase == Phase.INTERRUPTING && prompt.turnID == current.activeMessageID) return
                        require(current.phase == Phase.RUNNING && prompt.turnID == current.activeMessageID)
                        val old = current.pending.firstOrNull { it.reference == prompt.reference }
                        if (old != null) require(old.metadataJSON == prompt.metadataJSON)
                        else { require(current.pending.size < 32); current = current.copy(pending = current.pending + prompt) }
                    }
                    "resolved", "expired" -> {
                        val reference = reference(event.getJSONObject("reference"))
                        current = current.copy(pending = current.pending.filterNot { it.reference == reference })
                    }
                    "closed" -> current = current.copy(pending = emptyList())
                    else -> error("Unknown Claude prompt event")
                }
            }
            "session" -> {
                val event = result.getJSONObject("event")
                val sequence = event.opt("sequence")
                require((sequence is Int || sequence is Long) && (sequence as Number).toLong() == sessionSequence + 1)
                sessionSequence++
                when (event.opt("kind")) {
                    "sdk-message" -> consumeMessage(event.getJSONObject("message"))
                    "interrupt-receipt" -> {
                        val id = identity(event.opt("messageID")); val active = id == current.activeMessageID
                        require(active || id in finishedInputs)
                        append("receipt:$sessionSequence", if (active) "已收到中断回执 · 等待最终结果" else "先前输入的中断回执", event.opt("receipt").toString())
                    }
                    "closed" -> ended(when (event.opt("reason")) { "stopped" -> Phase.CLOSED; "failed" -> Phase.FAILED; "lost" -> Phase.LOST; else -> error("Unknown close") })
                    else -> error("Unknown Claude session event")
                }
            }
            else -> error("Unknown Claude event source")
        }
    }
    private fun ended(next: Phase) {
        current = current.copy(phase = next, pending = emptyList(), issue = if (current.activeMessageID != null)
            "后端会话已结束；当前输入的结果尚未确认。" else if (next != Phase.CLOSED) "后端初始化或连接失败；请检查执行端配置和依赖。" else current.issue)
        valid = false; polling?.cancel(); polling = null
    }
    private fun consumeMessage(message: JSONObject) {
        val type = message.opt("type") as? String ?: error("Missing Claude message type")
        when (type) {
            "result" -> {
                val id = message.opt("user_message_uuid") as? String
                val error = message.opt("is_error") as? Boolean
                val subtype = message.opt("subtype") as? String
                if (id == null || id != current.activeMessageID || error == null || subtype == null || (subtype == "success") == error) {
                    fail("收到无法关联当前输入的后端结果；当前输入保持未知。", Phase.LOST); return
                }
                append("result:$sessionSequence", if (error) "本轮未成功" else "本轮已完成",
                    message.opt("result") as? String ?: message.opt("errors")?.toString() ?: message.toString())
                finishedInputs += id; if (finishedInputs.size > 64) finishedInputs.removeAt(0)
                current = current.copy(activeMessageID = null, pending = emptyList(), phase = Phase.READY)
            }
            "stream_event" -> {
                val event = message.optJSONObject("event"); val delta = event?.optJSONObject("delta"); val index = event?.opt("index") as? Int
                val turn = current.activeMessageID
                if (event?.opt("type") == "content_block_delta" && delta?.opt("type") == "text_delta" &&
                    index != null && index in 0..4096 && turn != null && delta.opt("text") is String) {
                    val id = "stream:$turn:$index"
                    append(id, "Claude · 正在生成", current.entries.firstOrNull { it.id == id }?.text.orEmpty() + delta.getString("text"))
                } else append("sdk:$sessionSequence", "Claude · $type", message.toString())
            }
            "assistant" -> {
                val contents = message.optJSONObject("message")?.optJSONArray("content")
                val text = contents?.let { (0 until it.length()).mapNotNull { index -> it.optJSONObject(index)?.takeIf { item -> item.opt("type") == "text" }?.opt("text") as? String }.joinToString("\n") }
                current.activeMessageID?.let { turn -> current = current.copy(entries = current.entries.filterNot { it.id.startsWith("stream:$turn:") }) }
                append("sdk:$sessionSequence", "Claude", text?.takeIf { it.isNotEmpty() } ?: message.toString())
            }
            else -> append("sdk:$sessionSequence", "Claude · $type", message.toString())
        }
    }
    private fun phase(value: Any?): Phase = Phase.entries.firstOrNull { it.name.lowercase(Locale.ROOT) == value } ?: error("Unknown Claude phase")
    private fun identity(value: Any?): String = (value as? String)?.also { require(it.isNotEmpty() && ClaudeJson.utf8(it).size <= 128) } ?: error("Missing Claude identity")
    private fun reference(value: JSONObject) = Reference(identity(value.opt("scope")), identity(value.opt("requestID")), identity(value.opt("token")))
    private fun prompt(value: JSONObject): Prompt {
        val reference = reference(value.getJSONObject("reference")); val tool = identity(value.opt("toolName")); val turn = identity(value.opt("turnID"))
        val input = value.getJSONObject("input"); require(ClaudeJson.utf8(input.toString()).size <= 65536)
        val isQuestion = tool == "AskUserQuestion"
        require(value.opt("kind") == if (isQuestion) "question" else "tool")
        val questions = if (isQuestion) questions(value.getJSONArray("questions")) else emptyList()
        if (isQuestion) require(questions(input.getJSONArray("questions")) == questions)
        return Prompt(reference, tool, turn, input.toString(), value.toString(), questions)
    }
    private fun questions(values: JSONArray): List<Question> {
        require(values.length() in 1..4)
        val keys = mutableSetOf<String>()
        fun text(value: Any?, limit: Int): String = (value as? String)?.also { require(ClaudeJson.utf8(it).size <= limit) } ?: error("Invalid Claude question string")
        return (0 until values.length()).map { index ->
            val value = values.getJSONObject(index); val question = text(value.opt("question"), 2048); val header = text(value.opt("header"), 128)
            require(question.isNotEmpty() && keys.add(question) && header.isNotEmpty() && header.codePointCount(0, header.length) <= 12)
            val multi = value.opt("multiSelect") as? Boolean ?: error("Invalid Claude question selection mode")
            val options = value.getJSONArray("options"); require(options.length() in 2..4); val labels = mutableSetOf<String>()
            Question(question, header, multi, (0 until options.length()).map { option ->
                val item = options.getJSONObject(option); val label = text(item.opt("label"), 256)
                require(label.isNotEmpty() && labels.add(label)); Option(label, text(item.opt("description"), 2048))
            })
        }
    }
}
