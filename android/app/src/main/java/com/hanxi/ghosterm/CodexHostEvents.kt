package com.hanxi.ghosterm

import com.hanxi.ghosterm.bridge.ExtensionSession
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import org.json.JSONTokener
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction

internal data class CodexDiffLine(val index: Int, val text: String, val kind: Kind) {
    enum class Kind { CONTEXT, ADDED, REMOVED, HUNK }
}
internal data class CodexFileChange(val path: String, val kind: String, val movePath: String?,
    val lines: List<CodexDiffLine>, val diffTruncated: Boolean)
internal data class CodexTimelineEntry(val id: String, val title: String, val detail: String,
    val fileChanges: List<CodexFileChange> = emptyList(), val omittedChangeCount: Int = 0,
    val changeReference: CodexChangeReference? = null, val changeUnavailable: Boolean = false)
internal data class CodexPendingApproval(val id: String, val method: String, val paramsJSON: String,
    val itemID: String?, val identity: CodexApprovalIdentity? = null, val permissionReviewID: String? = null)
internal data class CodexHostState(
    val sequence: Long = 0,
    val rawEvents: List<String> = emptyList(),
    val timeline: List<CodexTimelineEntry> = emptyList(),
    val approvals: List<CodexPendingApproval> = emptyList(),
    val threadID: String? = null,
    val messageDraft: String = "",
    val callBusy: Boolean = false,
    val turnActive: Boolean = false,
    val sendUnconfirmed: Boolean = false,
)

internal data class CodexHostEvent(
    val sequence: Long,
    val kind: String,
    val encoded: String,
    val method: String? = null,
    val params: Any? = null,
    val approval: CodexPendingApproval? = null,
    val resolvedRequestID: String? = null,
)

/** Decoder and bounded reducer for the locked ai-codex Executor event envelope. */
internal object CodexHostEvents {
    private const val MAX_EVENT_BYTES = 3072
    private const val MAX_EVENTS = 128
    private const val MAX_HISTORY_BYTES = 262144
    private const val MAX_APPROVALS = 32
    private const val MAX_ENTRY_BYTES = 32768
    private const val SAFE_INTEGER = 9007199254740991L

    private val eventKinds = setOf("notification", "approval-request", "approval-resolved",
        "outcome-unknown", "protocol-error", "closed")
    private val approvalMethods = setOf("item/commandExecution/requestApproval", "item/fileChange/requestApproval", "item/permissions/requestApproval")

    class ConfirmedNoEffect(message: String) : IllegalStateException(message)

    fun decodeOutcome(bytes: ByteArray): JSONObject {
        val outcome = parse(strictUtf8(bytes)) as? JSONObject ?: error("Codex RPC outcome must be an object")
        if (outcome.optString("kind") == "failed" && outcome.optJSONObject("error")?.optString("effect") == "none") {
            throw ConfirmedNoEffect(outcome.getJSONObject("error").optString("message", "Executor 未执行此请求"))
        }
        require(outcome.optString("kind") == "success") {
            val failure = outcome.optJSONObject("error")
            "Executor RPC failed: ${failure?.optString("message").orEmpty().ifBlank { outcome.optString("kind") }}"
        }
        return outcome.optJSONObject("value") ?: error("Codex RPC success value is missing")
    }

    fun threadID(value: JSONObject): String {
        val id = value.opt("threadID") as? String
        require(id != null && id.isNotBlank() && codexChangeUtf8(id).size <= 128) { "Codex thread ID 无效" }
        return id
    }

    fun decode(request: ExtensionSession.RPCRequest): CodexHostEvent {
        require(request.method == "codex.event") { "Host method is not registered" }
        val source = strictUtf8(request.input)
        val envelope = parse(source) as? JSONObject ?: error("Codex event input must be an object")
        val encoded = envelope.opt("event") as? String ?: error("Codex event string is missing")
        require(encoded.toByteArray(Charsets.UTF_8).size <= MAX_EVENT_BYTES) { "Codex event exceeds the size limit" }
        val event = parse(encoded) as? JSONObject ?: error("Codex event must be an object")
        val sequence = safeInteger(event.opt("sequence")) ?: error("Codex event sequence is invalid")
        require(sequence > 0) { "Codex event sequence is invalid" }
        val kind = event.optString("kind", "")
        require(kind in eventKinds) { "Unknown Codex event kind" }
        var method: String? = null
        var params: Any? = null
        var approval: CodexPendingApproval? = null
        var resolved: String? = null
        when (kind) {
            "notification" -> {
                val notification = event.optJSONObject("notification") ?: error("Codex notification is missing")
                method = notification.optString("method", "").also { require(it.isNotBlank()) }
                params = notification.opt("params").takeUnless { it === JSONObject.NULL }
            }
            "approval-request" -> {
                val requestObject = event.optJSONObject("request") ?: error("Codex approval request is missing")
                val id = requireNotNull(requestKey(requestObject.opt("id"))) { "Codex approval request ID is invalid" }
                val requestMethod = requestObject.optString("method", "")
                require(requestMethod in approvalMethods) { "Unsupported Codex approval method" }
                val requestParams = requestObject.opt("params")
                require(requestParams is JSONObject || requestParams is JSONArray) { "Codex approval params are invalid" }
                val itemID = (requestParams as? JSONObject)?.optString("itemId")
                    ?.takeIf { it.isNotBlank() && it.toByteArray(Charsets.UTF_8).size <= 128 }
                val identity = if (requestMethod == "item/fileChange/requestApproval") {
                    val fields = requestParams as? JSONObject
                    fun part(name: String): String = requireNotNull(fields?.opt(name) as? String).also {
                        require(it.isNotEmpty() && codexChangeUtf8(it).size <= 128) { "Codex approval identity is invalid" }
                    }
                    CodexApprovalIdentity(part("threadId"), part("turnId"), part("itemId"))
                } else null
                val permissionReviewID = if (requestMethod == "item/permissions/requestApproval") {
                    val fields = requestParams as? JSONObject ?: error("Permission params must be an object")
                    for (name in listOf("threadId", "turnId", "itemId")) {
                        val part = fields.opt(name) as? String ?: error("Permission identity is invalid")
                        require(part.isNotEmpty() && codexChangeUtf8(part).size <= 128)
                    }
                    val profile = fields.optJSONObject("permissions") ?: error("Permission profile is missing")
                    require(profile.keys().asSequence().toSet() == setOf("network", "fileSystem"))
                    require(listOf("network", "fileSystem").all { profile.opt(it) === JSONObject.NULL || profile.opt(it) is JSONObject })
                    require(fields.opt("cwd") is String)
                    (requestObject.opt("permissionReviewID") as? String)?.also {
                        require(Regex("permissions\\.[1-9][0-9]{0,19}").matches(it))
                    } ?: error("Permission review reference is missing")
                } else null
                approval = CodexPendingApproval(id, requestMethod, prettyJSON(requestParams), itemID, identity, permissionReviewID)
            }
            "approval-resolved" -> resolved = requestKey(event.opt("requestID"))
        }
        return CodexHostEvent(sequence, kind, encoded, method, params, approval, resolved)
    }

    fun accept(state: CodexHostState, event: CodexHostEvent): CodexHostState {
        require(state.sequence < Long.MAX_VALUE && event.sequence == state.sequence + 1) {
            "Codex event sequence is discontinuous"
        }
        require(event.encoded.toByteArray(Charsets.UTF_8).size <= MAX_EVENT_BYTES) { "Codex event exceeds the size limit" }
        // Raw history is a recent diagnostic window, not the sequence ledger
        // or approval owner. Retiring its oldest payload cannot authorize a
        // replay or resolve a pending approval.
        val recent = (state.rawEvents + event.encoded).toMutableList()
        var retained = recent.sumOf { it.toByteArray(Charsets.UTF_8).size }
        var removed = 0
        while (recent.size - removed > MAX_EVENTS || retained > MAX_HISTORY_BYTES) {
            retained -= recent[removed].toByteArray(Charsets.UTF_8).size
            removed++
        }
        if (removed != 0) recent.subList(0, removed).clear()
        val approvals = state.approvals.toMutableList()
        when (event.kind) {
            "approval-request" -> {
                val request = checkNotNull(event.approval)
                require(approvals.none { it.id == request.id } && approvals.size < MAX_APPROVALS) {
                    "Codex approval request is duplicate or exceeds the limit"
                }
                approvals += request
            }
            "approval-resolved" -> event.resolvedRequestID?.let { id -> approvals.removeAll { it.id == id } }
        }
        if ((event.kind == "notification" && event.method == "turn/completed") ||
            event.kind in setOf("protocol-error", "closed", "outcome-unknown")) {
            approvals.clear()
        }
        var turnActive = state.turnActive
        var timeline = state.timeline
        if (event.kind == "notification") {
            if (event.method == "turn/started") turnActive = true
            if (event.method == "turn/completed") turnActive = false
            if (event.params != null) timeline = reduceTimeline(timeline, event.sequence,
                checkNotNull(event.method), event.params)
        } else if (event.kind != "approval-request" && event.kind != "approval-resolved") {
            timeline = reduceTimeline(timeline, event.sequence, event.kind, parse(event.encoded) as JSONObject)
        }
        return state.copy(sequence = event.sequence, rawEvents = recent,
            timeline = timeline, approvals = approvals, turnActive = turnActive)
    }

    fun commitAcknowledged(
        previous: CodexHostState,
        validated: CodexHostState,
        latest: CodexHostState,
        event: CodexHostEvent,
    ): CodexHostState = if (latest === previous) validated else accept(latest, event)

    private fun reduceTimeline(entries: List<CodexTimelineEntry>, sequence: Long,
        method: String, value: Any): List<CodexTimelineEntry> {
        val objectValue = value as? JSONObject
        if (method == "serverRequest/resolved") return entries
        if (method == "protocol-error") return replace(entries,
            CodexTimelineEntry("event:$sequence", "Codex 协议错误", objectValue?.optString("message").orEmpty().ifBlank { prettyJSON(value) }))
        if (method == "outcome-unknown") return replace(entries, CodexTimelineEntry("event:$sequence", "执行结果未知", prettyJSON(value)))
        if (method == "closed") return replace(entries,
            CodexTimelineEntry("event:$sequence", "Codex 会话已关闭", objectValue?.optString("outcome").orEmpty()))

        val item = objectValue?.optJSONObject("item")
        if ((method == "item/started" || method == "item/completed") && item != null) {
            val itemID = item.optString("id", "")
            val type = item.optString("type", "")
            if (itemID.isNotEmpty() && type.isNotEmpty()) {
                if (type == "fileChange") {
                    val marker = item.optJSONObject("_ghosterm")
                    return replace(entries, CodexTimelineEntry("item:$itemID", "文件变更", "",
                        fileChanges = fileChanges(item.opt("changes"), marker),
                        omittedChangeCount = (marker?.optInt("omittedChanges", 0)?.coerceAtLeast(0) ?: 0) +
                            ((item.optJSONArray("changes")?.length() ?: 0) - 8).coerceAtLeast(0),
                        changeReference = CodexChangeJson.reference(marker, objectValue.opt("threadId"),
                            objectValue.opt("turnId"), itemID), changeUnavailable = marker?.has("snapshotUnavailable") == true))
                }
                val previous = entries.firstOrNull { it.id == "item:$itemID" }?.detail
                val (title, detail) = when (type) {
                    "agentMessage" -> "Codex" to item.optString("text", previous.orEmpty())
                    "commandExecution" -> "命令执行" to commandDetail(item, previous)
                    "fileChange" -> "文件变更" to prettyJSON(item)
                    "reasoning" -> "Reasoning" to "推理内容不在插件面板展示。"
                    else -> type to prettyJSON(item)
                }
                return replace(entries, CodexTimelineEntry("item:$itemID", title, detail))
            }
        }
        if ((method == "item/agentMessage/delta" || method == "item/commandExecution/outputDelta") && objectValue != null) {
            val itemID = objectValue.optString("itemId", "")
            val delta = objectValue.optString("delta", "")
            if (itemID.isNotEmpty()) {
                val previous = entries.firstOrNull { it.id == "item:$itemID" }
                val title = previous?.title ?: if (method == "item/agentMessage/delta") "Codex" else "命令输出"
                val old = previous?.detail.orEmpty()
                val detail = if (method == "item/commandExecution/outputDelta") {
                    old + (if (old.contains("\n输出：\n")) "" else if (old.isEmpty()) "输出：\n" else "\n输出：\n") + delta
                } else old + delta
                return replace(entries, CodexTimelineEntry("item:$itemID", title, detail))
            }
        }
        if (method == "item/fileChange/patchUpdated" && objectValue != null) {
            val itemID = objectValue.optString("itemId", "")
            if (itemID.isNotEmpty()) {
                val marker = objectValue.optJSONObject("_ghosterm")
                return replace(entries, CodexTimelineEntry("item:$itemID", "文件变更", "",
                    fileChanges(objectValue.opt("changes"), marker),
                    (marker?.optInt("omittedChanges", 0)?.coerceAtLeast(0) ?: 0) +
                        ((objectValue.optJSONArray("changes")?.length() ?: 0) - 8).coerceAtLeast(0),
                    changeReference = CodexChangeJson.reference(marker, objectValue.opt("threadId"),
                        objectValue.opt("turnId"), itemID), changeUnavailable = marker?.has("snapshotUnavailable") == true))
            }
        }
        val title = when (method) {
            "turn/started" -> "Turn 已启动"
            "turn/completed" -> "Turn 已完成"
            else -> method
        }
        val detail = if (method.startsWith("turn/")) {
            val turn = objectValue?.optJSONObject("turn")
            val id = turn?.optString("id", "").orEmpty()
            val status = turn?.optString("status", "unknown").orEmpty()
            if (id.isEmpty()) status else "$id · $status"
        } else prettyJSON(value)
        return replace(entries, CodexTimelineEntry("event:$sequence", title, detail))
    }

    private fun commandDetail(item: JSONObject, previous: String?): String = buildList {
        item.optString("command", "").takeIf { it.isNotEmpty() }?.let(::add)
        item.optString("cwd", "").takeIf { it.isNotEmpty() }?.let { add("目录：$it") }
        item.optString("status", "").takeIf { it.isNotEmpty() }?.let { add("状态：$it") }
        if (item.has("exitCode") && item.opt("exitCode") !== JSONObject.NULL) add("退出码：${item.opt("exitCode")}")
        val output = item.optString("aggregatedOutput", "")
        if (output.isNotEmpty()) add("输出：\n$output")
        else if (!previous.isNullOrEmpty()) {
            val marker = "\n输出：\n"
            val start = previous.indexOf(marker)
            if (start >= 0) add("输出：\n" + previous.substring(start + marker.length))
        }
    }.joinToString("\n")

    private fun fileChanges(value: Any?, marker: JSONObject?): List<CodexFileChange> {
        val source = value as? JSONArray ?: return emptyList()
        val result = mutableListOf<CodexFileChange>()
        for (index in 0 until minOf(source.length(), 8)) {
            val change = source.optJSONObject(index) ?: continue
            val path = change.optString("path", "")
            val kindObject = change.optJSONObject("kind") ?: continue
            val kind = kindObject.optString("type", "")
            val diff = change.optString("diff", "")
            if (path.isBlank() || path.toByteArray(Charsets.UTF_8).size > 512 || kind !in setOf("add", "delete", "update")) continue
            val movePath = if (kind == "update") kindObject.optString("move_path").takeIf { it.isNotBlank() } else null
            val allLines = diff.split('\n')
            val lines = allLines.take(256).mapIndexed { lineIndex, text ->
                val lineKind = when {
                    text.startsWith("@@") -> CodexDiffLine.Kind.HUNK
                    text.startsWith('+') -> CodexDiffLine.Kind.ADDED
                    text.startsWith('-') -> CodexDiffLine.Kind.REMOVED
                    else -> CodexDiffLine.Kind.CONTEXT
                }
                CodexDiffLine(lineIndex, text, lineKind)
            }
            result += CodexFileChange(path, kind, movePath, lines,
                allLines.size > lines.size || diff.contains("…（diff 已截断）") || marker?.optBoolean("diffTruncated", false) == true)
        }
        return result
    }

    private fun replace(entries: List<CodexTimelineEntry>, incoming: CodexTimelineEntry): List<CodexTimelineEntry> {
        val bounded = incoming.copy(title = bounded(incoming.title), detail = bounded(incoming.detail))
        val result = entries.toMutableList()
        val index = result.indexOfFirst { it.id == bounded.id }
        if (index >= 0) result[index] = bounded else result += bounded
        while (result.size > MAX_EVENTS || result.sumOf { entry ->
                entry.title.toByteArray(Charsets.UTF_8).size + entry.detail.toByteArray(Charsets.UTF_8).size +
                    entry.fileChanges.sumOf { file -> file.path.toByteArray(Charsets.UTF_8).size +
                        (file.movePath?.toByteArray(Charsets.UTF_8)?.size ?: 0) +
                        file.lines.sumOf { it.text.toByteArray(Charsets.UTF_8).size } }
            } > MAX_HISTORY_BYTES) {
            val remove = result.indexOfFirst { it.id != bounded.id }.takeIf { it >= 0 } ?: result.lastIndex
            if (remove < 0) break
            result.removeAt(remove)
        }
        return result
    }

    private fun bounded(value: String): String = if (value.toByteArray(Charsets.UTF_8).size <= MAX_ENTRY_BYTES) value
        else value.take(8000) + "\n…（内容已截断）"

    private fun prettyJSON(value: Any): String = try {
        when (value) {
            is JSONObject -> value.toString(2)
            is JSONArray -> value.toString(2)
            else -> value.toString()
        }
    } catch (_: JSONException) { "" }

    private fun requestKey(value: Any?): String? = when (value) {
        is String -> value.takeIf { it.isNotEmpty() && it.toByteArray(Charsets.UTF_8).size <= 128 }?.let { "s:$it" }
        is Number -> safeInteger(value)?.let { "n:$it" }
        else -> null
    }

    private fun safeInteger(value: Any?): Long? {
        val number = value as? Number ?: return null
        val double = number.toDouble()
        if (!double.isFinite() || double % 1.0 != 0.0 || kotlin.math.abs(double) > SAFE_INTEGER.toDouble()) return null
        return double.toLong()
    }

    private fun strictUtf8(bytes: ByteArray): String = Charsets.UTF_8.newDecoder()
        .onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
        .decode(ByteBuffer.wrap(bytes)).toString()

    private fun parse(source: String): Any? {
        val tokener = JSONTokener(source)
        val value = tokener.nextValue()
        require(tokener.nextClean() == '\u0000') { "Trailing Codex JSON data" }
        return value
    }
}
