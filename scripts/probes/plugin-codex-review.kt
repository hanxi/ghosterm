@file:JvmName("CodexReviewFixture")
package com.hanxi.ghosterm

import kotlinx.coroutines.*

private fun expect(value: Boolean, label: String) { if (!value) throw AssertionError(label) }
private fun rejected(label: String, block: () -> Unit) {
    try { block() } catch (_: Exception) { return }
    throw AssertionError(label)
}
private val text = "{\"diff\":\"" + "中文😀e\u0301\\n".repeat(1000) + "\"}"
private fun ref(id: String = "change-1", content: String = text): CodexChangeReference = checkNotNull(
    CodexChangeReference.create(id, "thread", "turn", "item", content.length.toLong(), codexChangeUtf8(content).size.toLong()))
private fun chunks(reference: CodexChangeReference, content: String = text): List<CodexChangeChunk> {
    val result = mutableListOf<CodexChangeChunk>()
    var offset = 0
    while (offset < content.length) {
        var end = (offset + 128).coerceAtMost(content.length)
        if (end < content.length && Character.isHighSurrogate(content[end - 1]) && Character.isLowSurrogate(content[end])) end--
        result += CodexChangeChunk("ok", reference.snapshotID, offset, end, reference.totalUnits,
            reference.totalBytes, content.substring(offset, end), end == content.length)
        offset = end
    }
    return result
}
private fun doc(bytes: ByteArray, reference: CodexChangeReference): CodexChangeDocument {
    expect(bytes.size == reference.totalBytes, "decoder must receive every byte")
    return CodexChangeDocument(reference.threadID, reference.turnID, reference.itemID,
        listOf(CodexCompleteChangeFile("fixture", "update", null, bytes.toString(Charsets.UTF_8))))
}

fun main(arguments: Array<String>) = runBlocking {
    val reference = ref()
    if (arguments.singleOrNull() == "control") {
        expect(reference.snapshotID == "change-1" && CodexChangeReference.create("invalid", "t", "u", "i", 1, 1) == null,
            "independent reference factory control")
        println("PASS independent reference control")
        return@runBlocking
    }
    expect(listOf("change-0", "change-01", "change-+1", "change-9007199254740992").all {
        CodexChangeReference.create(it, "t", "u", "i", 1, 1) == null
    }, "snapshot IDs must be canonical")
    expect(CodexChangeReference.create("change-1", "t", "u", "i", 1, 524289) == null, "snapshot byte budget must be enforced")
    rejected("malformed surrogate must not become replacement UTF8 bytes") { codexChangeUtf8("\uD800") }

    val parts = chunks(reference)
    val assembler = CodexChangeAssembler(reference)
    rejected("partial document must not be exposed") { assembler.finishBytes() }
    for (chunk in parts) assembler.append(chunk)
    expect(assembler.finishBytes().contentEquals(codexChangeUtf8(text)), "all Unicode bytes must survive assembly")
    expect(assembler.nextOffset == text.length, "offsets must use UTF16")
    rejected("data after EOF must be rejected") { assembler.append(parts.first()) }
    rejected("snapshot identity must match") { CodexChangeAssembler(reference).append(parts.first().copy(snapshotID = "change-2")) }
    rejected("chunks must be contiguous") { CodexChangeAssembler(reference).append(parts.first().copy(offset = 1)) }
    rejected("UTF16 chunk length must match") { CodexChangeAssembler(reference).append(parts.first().copy(nextOffset = 1)) }
    for (bad in listOf(parts.first().copy(status = "expired"), parts.first().copy(status = "invalid"),
        parts.first().copy(totalBytes = reference.totalBytes + 1), parts.first().copy(done = true),
        parts.first().copy(content = "", nextOffset = 0))) {
        rejected("invalid chunk must be rejected") { CodexChangeAssembler(reference).append(bad) }
    }
    val wrongBytes = reference.copy(totalBytes = reference.totalBytes + 1)
    val wrongAssembler = CodexChangeAssembler(wrongBytes)
    val wrongParts = chunks(wrongBytes)
    for (chunk in wrongParts.dropLast(1)) wrongAssembler.append(chunk)
    rejected("EOF must match actual UTF8 bytes") { wrongAssembler.append(wrongParts.last()) }
    println("PASS assembly: exact Unicode / UTF16 / identity / offset / bytes / EOF / limits")

    val scope = CoroutineScope(coroutineContext + SupervisorJob(coroutineContext[Job]))
    try {
        val controller = CodexChangeReviewController(scope)
        val job = controller.load(reference, { true }, { offset -> parts.first { it.offset == offset } }, ::doc)
        job.join()
        expect(controller.state.value?.document?.files?.single()?.diff == text, "complete document must be published")

        val entered = CompletableDeferred<Unit>()
        val release = CompletableDeferred<CodexChangeChunk>()
        val old = controller.load(reference, { true }, {
            withContext(NonCancellable) { entered.complete(Unit); release.await() }
        }, ::doc)
        entered.await()
        controller.cancel()
        val replacement = ref("change-2", "replacement")
        val replacementParts = chunks(replacement, "replacement")
        val newer = controller.load(replacement, { true }, { offset -> replacementParts.first { it.offset == offset } }, ::doc)
        newer.join()
        release.complete(parts.first()); old.join()
        expect(controller.state.value?.reference == replacement && controller.state.value?.document != null,
            "cancelled old callbacks must not replace a newer review")

        var current = true
        val waiting = CompletableDeferred<Unit>()
        val resume = CompletableDeferred<CodexChangeChunk>()
        val stale = controller.load(reference, { current }, { offset ->
            if (offset == 0) { waiting.complete(Unit); resume.await() }
            else parts.first { it.offset == offset }
        }, ::doc)
        waiting.await(); current = false; resume.complete(parts.first()); stale.join()
        expect(controller.state.value == null, "changed reference must discard late data")
        val failed = controller.load(reference, { true }, { parts.first().copy(status = "expired") }, ::doc)
        failed.join()
        expect(controller.state.value?.issue != null && controller.state.value?.document == null, "expired reply must never publish a document")
        controller.cancel()
        expect(controller.state.value == null, "closing review must clear complete data")

        val timed = CodexChangeReviewController(scope, timeoutMillis = 20)
        timed.load(reference, { true }, { delay(1000); parts.first() }, ::doc).join()
        expect(timed.state.value?.issue != null && timed.state.value?.document == null, "timeout must be visible without partial publication")
        current = true
        timed.load(reference, { current }, { current = false; delay(1000); parts.first() }, ::doc).join()
        expect(timed.state.value == null, "timeout on a stale reference must clear loading")
        controller.load(reference, { true }, { offset -> parts.first { it.offset == offset } },
            { _, _ -> error("invalid complete document") }).join()
        expect(controller.state.value?.document == null && controller.state.value?.issue == "invalid complete document",
            "decoder failure must not publish partial data")
        println("PASS lifecycle: completion / cancel / late callback / replacement / scope / expired / timeout")
    } finally { scope.cancel() }
    println("ANDROID CODEX REVIEW JVM PASS")
}
