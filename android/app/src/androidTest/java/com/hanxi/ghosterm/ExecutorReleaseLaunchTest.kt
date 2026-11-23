package com.hanxi.ghosterm

import org.junit.Assert.*
import org.junit.Test

/** Pure startup-contract checks; also runnable on a host JVM with android.jar. */
class ExecutorReleaseLaunchTest {
    private val digest = "a".repeat(64)
    private val evidence = ExtensionRelayBootstrap.ReleaseEvidence("/release/中'😀", "/policy/中'😀.json", digest, true)
    private fun request(proof: ExtensionRelayBootstrap.ReleaseEvidence? = evidence) = ExtensionRelayBootstrap.Request(
        "/selected/gterm", "/selected/package", digest, digest, emptyList(), command = "fixture",
        arguments = listOf("", "中'😀"), releaseEvidence = proof)
    private fun reject(block: () -> Unit) {
        try { block(); fail("Signed selection must fail before opening SSH, without a development fallback") }
        catch (_: IllegalArgumentException) { }
    }
    private fun decode(hex: String): List<String> {
        val bytes = hex.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
        var at = 1
        fun u16(): Int { val result = (bytes[at].toInt() and 255) * 256 + (bytes[at + 1].toInt() and 255); at += 2; return result }
        assertEquals(1, bytes[0].toInt())
        val values = List(u16()) { val size = u16(); bytes.copyOfRange(at, at + size).toString(Charsets.UTF_8).also { at += size } }
        assertEquals(bytes.size, at); return values
    }
    @Test fun evidenceSurvivesShellStartupGroupAndOnlineProfile() {
        val request = request()
        val command = ExtensionRelayBootstrap.build(request)
        assertTrue(command.contains("'--release-evidence' '/release/中'\\''😀' '/policy/中'\\''😀.json' '$digest'"))
        val profile = ExtensionRelayBootstrap.multiplexProfile(request)
        assertEquals(listOf("--trust-development-digest", digest, "--client-digest", digest,
            "--release-evidence", evidence.directory, evidence.policyPath, digest,
            "--", request.executorDirectory, "fixture", "", "中'😀"), decode(profile))
        val group = ExtensionRelayBootstrap.buildMultiplex("c".repeat(32),
            listOf(ExtensionController.MultiplexScope("b".repeat(32), 1uL)), listOf(request), "/owned/tmp", live = true)
        assertTrue(group.endsWith("'$profile'"))
        assertEquals(evidence, request.copy(methods = listOf("fixture.echo")).releaseEvidence)
        assertFalse(ExtensionRelayBootstrap.build(request(null)).contains("--release-evidence"))
    }
    @Test fun pinnedVariantIsKeptWithLuaAndTheSignedProfile() {
        val selected = request(null).copy(variantID = "lua")
        val command = ExtensionRelayBootstrap.build(selected)
        assertTrue(command.contains("'--variant' 'lua'"))
        assertFalse(command.contains("--allow-process-execution"))
        val profile = ExtensionRelayBootstrap.multiplexProfile(selected)
        assertEquals(listOf("--variant", "lua"), decode(profile).subList(4, 6))
        val group = ExtensionRelayBootstrap.buildMultiplex("c".repeat(32),
            listOf(ExtensionController.MultiplexScope("b".repeat(32), 1uL)), listOf(selected), "/stage", live = true)
        assertTrue(group.endsWith("'$profile'"))
        assertEquals(selected.variantID, selected.copy(methods = listOf("fixture.echo")).variantID)
        val signed = request().copy(variantID = "signed-variant")
        assertEquals(evidence, signed.copy(arguments = listOf("new")).releaseEvidence)
        val argv = decode(ExtensionRelayBootstrap.multiplexProfile(signed))
        assertTrue(argv.contains("--release-evidence")); assertTrue(argv.contains("signed-variant"))
        assertFalse(ExtensionRelayBootstrap.build(request()).contains("--variant"))
    }
    @Test fun invalidVariantAndForegroundReconfigurationNeverStartOrGrantProcessExecution() {
        for (id in listOf("", " ", "with space", "a/b", "\"injection", "x;touch BAD", "\u0000", "line\nfeed", "变体", "a".repeat(129))) {
            reject { ExtensionRelayBootstrap.build(request(null).copy(variantID = id)) }
            reject { ExtensionRelayBootstrap.multiplexProfile(request(null).copy(variantID = id)) }
        }
        ExtensionRelayBootstrap.build(request(null).copy(variantID = "a".repeat(128)))
        reject { ExtensionRelayBootstrap.build(request(null).copy(variantID = "new", executorDirectory = "",
            attachment = ExtensionRelayBootstrap.Attachment("/front", "b".repeat(32), true))) }
    }
    @Test fun invalidOrUnapprovedEvidenceNeverStartsAndCannotAlterForegroundOwner() {
        val invalid = mutableListOf(evidence.copy(authorized = false))
        for (path in listOf("", "relative", "/", "/r/", "/r//x", "/r/../x", "/r/./x", "/r\n", "/r\u0000", "/\uD800")) {
            invalid.add(evidence.copy(directory = path)); invalid.add(evidence.copy(policyPath = path))
        }
        for (digest in listOf("", "a", "A".repeat(64))) invalid.add(evidence.copy(policySHA256 = digest))
        invalid.forEach { proof ->
            reject { ExtensionRelayBootstrap.build(request(proof)) }
            reject { ExtensionRelayBootstrap.multiplexProfile(request(proof)) }
        }
        reject { ExtensionRelayBootstrap.build(request().copy(executorDirectory = "",
            attachment = ExtensionRelayBootstrap.Attachment("/front", "b".repeat(32), true))) }
    }
}
