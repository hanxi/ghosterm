package com.hanxi.ghosterm

import android.os.Looper
import java.io.ByteArrayOutputStream
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import org.json.JSONObject

/** Main-thread Native owner for the candidate's complete-document RPC. This
 * layer preserves vendor records; it does not decode them as Codex events. */
internal class ClaudeRPCClient(private val rpc: suspend (String, ByteArray) -> ByteArray) {
    companion object {
        val methods = listOf("claude.input.open", "claude.input.append", "claude.input.seal",
            "claude.input.discard", "claude.call", "claude.response.read", "claude.response.forget", "claude.status")
    }
    class Observation(val source: Source, val method: String, val inputID: String?, val responseID: String?,
        response: ByteArray?, val backendPhase: String? = null, val failure: String? = null,
        val exitJSON: String? = null, val reaped: Boolean? = null) {
        enum class Source { UNAVAILABLE, EXECUTOR, RECEIVED }
        private val record = response?.copyOf()
        val response get() = record?.copyOf()
        val text get() = record?.toString(Charsets.UTF_8)
    }
    private var scopeOpen = true
    private var lastMethod: String? = null
    private var dispatched = false
    private var completeResponse: ByteArray? = null
    private var valid = true
    private var busy = false
    private var sequence = 0L
    var lastInputID: String? = null
        private set
    var lastResponseID: String? = null
        private set

    fun invalidate() { main(); scopeOpen = false; valid = false }
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) { "Claude RPC requires its Main owner" } }
    private suspend fun checkCurrent() { main(); currentCoroutineContext().ensureActive(); check(valid) { "Claude RPC scope closed" } }

    suspend fun request(method: String, parameters: ByteArray): ByteArray {
        checkCurrent(); check(!busy) { "Claude RPC busy" }
        require(method in setOf("start", "send", "interrupt", "approve", "answer", "poll", "stop"))
        ClaudeJson.objectValue(parameters, 64000)
        val text = parameters.toString(Charsets.UTF_8)
        lastInputID = null; lastResponseID = null; completeResponse = null
        lastMethod = method; dispatched = false
        busy = true
        try {
            val opened = call("claude.input.open", JSONObject().put("totalUnits", text.length))
            ok(opened)
            val inputID = string(opened, "inputID")
            require(identifier(inputID, "input-") && count(opened, "nextOffset") == 0)
            lastInputID = inputID; lastResponseID = null
            var offset = 0
            while (offset < text.length) {
                var end = minOf(offset + 256, text.length)
                if (end < text.length && text[end - 1].isHighSurrogate()) end--
                val appended = call("claude.input.append", JSONObject().put("inputID", inputID)
                    .put("offset", offset).put("content", text.substring(offset, end)))
                ok(appended); require(count(appended, "nextOffset") == end)
                offset = end
            }
            ok(call("claude.input.seal", JSONObject().put("inputID", inputID)))
            dispatched = true
            val receipt = call("claude.call", JSONObject().put("method", method).put("inputID", inputID))
            ok(receipt)
            val responseID = string(receipt, "responseID")
            val units = count(receipt, "totalUnits"); val bytes = count(receipt, "totalBytes")
            require(identifier(responseID, "response-") && units in 1..600000 && bytes in units..600000)
            lastResponseID = responseID
            val raw = readResponse(responseID, units, bytes)
            completeResponse = raw.copyOf()
            val forgotten = call("claude.response.forget", JSONObject().put("responseID", responseID))
            require(boolean(forgotten, "accepted")) { "Claude receipt was not released" }
            sequence++
            return raw
        } catch (failure: Throwable) {
            valid = false
            throw failure
        } finally { busy = false }
    }

    /** Explicit observation never dispatches, forgets, advances sequence or
     * reopens effects. A missing receipt does not mean an action did not run. */
    suspend fun observe(): Observation {
        checkObservation(); check(!busy) { "Claude RPC busy" }
        check(!valid && lastMethod != null) { "No uncertain Claude operation" }
        busy = true
        try {
            fun snapshot(source: Observation.Source, response: ByteArray? = null, backend: JSONObject? = null) =
                Observation(source, checkNotNull(lastMethod), lastInputID, lastResponseID, response,
                    backend?.let { string(it, "phase") }, backend?.let { string(it, "failure") },
                    backend?.let { string(it, "exitJSON") }, backend?.let { boolean(it, "reaped") })
            completeResponse?.let { return snapshot(Observation.Source.RECEIVED, it) }
            val inputID = lastInputID
            if (!dispatched || inputID == null) return snapshot(Observation.Source.UNAVAILABLE)
            val backend = call("claude.status", JSONObject(), observing = true)
            val phase = string(backend, "phase"); val failure = string(backend, "failure"); val exit = string(backend, "exitJSON")
            boolean(backend, "reaped")
            require(phase.isNotEmpty() && ClaudeJson.utf8(phase).size <= 32 && ClaudeJson.utf8(failure).size <= 32 && ClaudeJson.utf8(exit).size <= 256)
            // Accept one JSON value, including null, through the strict object parser.
            val exitValue = ClaudeJson.objectValue(ClaudeJson.utf8("{\"exit\":" + exit + "}"))
            require(exitValue.length() == 1)
            val observedInput = string(backend, "inputID"); val responseID = string(backend, "responseID")
            if (observedInput.isEmpty() && responseID.isEmpty()) return snapshot(Observation.Source.UNAVAILABLE, backend = backend)
            require(observedInput == inputID && identifier(responseID, "response-") &&
                (lastResponseID == null || lastResponseID == responseID)) { "Claude observation identity mismatch" }
            lastResponseID = responseID
            val first = call("claude.response.read", JSONObject().put("responseID", responseID).put("offset", 0), observing = true)
            ok(first)
            val raw = readResponse(responseID, count(first, "totalUnits"), count(first, "totalBytes"), observing = true, first = first)
            checkObservation(); completeResponse = raw.copyOf()
            return snapshot(Observation.Source.EXECUTOR, raw, backend)
        } finally { busy = false }
    }
    private suspend fun checkObservation() {
        main(); currentCoroutineContext().ensureActive(); check(scopeOpen) { "Claude observation scope closed" }
    }
    private suspend fun readResponse(responseID: String, units: Int, bytes: Int,
        observing: Boolean = false, first: JSONObject? = null): ByteArray {
        require(identifier(responseID, "response-") && units in 1..600000 && bytes in units..600000)
        val result = ByteArrayOutputStream()
        var received = 0
        while (received < units) {
            val page = if (received == 0 && first != null) first else call("claude.response.read", JSONObject().put("responseID", responseID).put("offset", received), observing)
            ok(page)
            val content = string(page, "content"); val data = ClaudeJson.utf8(content)
            require(string(page, "responseID") == responseID && count(page, "offset") == received &&
                count(page, "totalUnits") == units && count(page, "totalBytes") == bytes &&
                content.length in 1..256 && count(page, "nextOffset") == received + content.length &&
                count(page, "nextOffset") <= units && boolean(page, "done") == (received + content.length == units) &&
                result.size() + data.size <= bytes) { "Claude reply page metadata mismatch" }
            result.write(data); received += content.length
        }
        val raw = result.toByteArray()
        require(raw.size == bytes) { "Claude reply byte count mismatch" }
        val response = ClaudeJson.objectValue(raw)
        val success = boolean(response, "ok")
        val expected = if (success) setOf("version", "id", "sequence", "ok", "result")
            else setOf("version", "id", "sequence", "ok", "error", "phase")
        val next = sequence + 1
        require(response.keys().asSequence().toSet() == expected && count(response, "version") == 1 &&
            integer(response, "sequence") == next && string(response, "id") == "command-$next" &&
            (success || string(response, "error") == "ACTION_REJECTED")) { "Claude child reply identity mismatch" }
        return raw
    }

    private suspend fun call(method: String, input: JSONObject, observing: Boolean = false): JSONObject {
        check(!observing || method in setOf("claude.status", "claude.response.read"))
        if (observing) checkObservation() else checkCurrent()
        val encoded = ClaudeJson.utf8(input.toString())
        require(encoded.size <= 3072)
        val result = rpc(method, encoded)
        if (observing) checkObservation() else checkCurrent()
        val envelope = ClaudeJson.objectValue(result, 3072)
        require(string(envelope, "kind") == "success") { "Claude document RPC was not successful" }
        return envelope.opt("value") as? JSONObject ?: error("Claude document RPC value missing")
    }
    private fun ok(value: JSONObject) { require(string(value, "status") == "ok") { "Claude document operation refused" } }
    private fun string(value: JSONObject, key: String): String = value.opt(key) as? String ?: error("Claude string missing: $key")
    private fun boolean(value: JSONObject, key: String): Boolean = value.opt(key) as? Boolean ?: error("Claude boolean missing: $key")
    private fun integer(value: JSONObject, key: String): Long = when (val number = value.opt(key)) {
        is Int -> number.toLong()
        is Long -> number
        else -> error("Claude integer missing: $key")
    }
    private fun count(value: JSONObject, key: String): Int = integer(value, key).also { require(it in 0..600000) }.toInt()
    private fun identifier(value: String, prefix: String): Boolean {
        if (!value.startsWith(prefix)) return false
        val number = value.removePrefix(prefix).toLongOrNull() ?: return false
        return number in 1..9007199254740991L && value == prefix + number
    }
}
