package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionPackage
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.security.MessageDigest

@RunWith(AndroidJUnit4::class)
class PluginReleaseDeclarationsTest {
    private val json = """{"sdkApiRange":"^1.0.0","bridgeProtocolRange":[1,2],"permissions":[{"side":"executor","capability":"process.spawn","purpose":"Full declared purpose without truncation.","scope":"instance","programs":["fixture-worker"]}],"executor":{"variants":[{"id":"node","runtime":"node","runtimeVersionRange":"^26.0.0","dependencies":[{"program":"fixture-worker","versionPolicy":"range","versionRange":"^1.2.0"}]}]}}"""
    private fun metadata(bytes: ByteArray) = ExtensionPackage.Metadata("org.ghosterm.fixture", "1.0.0", "fixture", "MIT", "client/index.js", "portable-js", "", "1.0.0",
        "a".repeat(64), MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) },
        listOf("session.rpc"), emptyList(), listOf(1), false, false, false, false)
    private fun decode(value: String) = value.toByteArray().let { PluginReleaseDeclarations.decode(it, metadata(it)) }
    private fun rejected(value: String) {
        try { decode(value); fail("invalid declaration accepted") } catch (_: IllegalArgumentException) { }
    }
    @Test fun allPermissionsProgramsAndExecutorDeclarationsRemainComplete() {
        val result = decode(json)
        assertEquals("^1.0.0", result.sdkRange); assertEquals("1.0.0", result.selectedSDK)
        assertEquals(listOf(1, 2), result.protocolRange); assertEquals(listOf(1), result.protocols)
        val permission = result.permissions.single()
        assertEquals("executor", permission.side); assertEquals("process.spawn", permission.capability)
        assertEquals("Full declared purpose without truncation.", permission.purpose); assertEquals("instance", permission.scope)
        assertEquals(listOf("fixture-worker"), permission.programs)
        assertNotNull("Executor declarations were discarded", result.executorJSON)
        val executor = JSONObject(result.executorJSON!!).getJSONArray("variants").getJSONObject(0)
        assertEquals("^26.0.0", executor.getString("runtimeVersionRange"))
        assertEquals("^1.2.0", executor.getJSONArray("dependencies").getJSONObject(0).getString("versionRange"))
    }
    @Test fun rawManifestMustMatchExactCoreDigest() {
        val bytes = json.toByteArray()
        try { PluginReleaseDeclarations.decode(bytes + byteArrayOf(10), metadata(bytes)); fail("unattested display bytes accepted") }
        catch (_: PluginReleaseDeclarations.Rejected) { }
        val tooLarge = json + " ".repeat(65536)
        rejected(tooLarge)
    }
    @Test fun duplicateAndMalformedJsonCannotOverrideDisplayedDeclarations() {
        rejected(json.replace("\"sdkApiRange\":\"^1.0.0\"", "\"sdkApiRange\":\"evil\",\"sdkApiRange\":\"^1.0.0\""))
        rejected("\uFEFF$json"); rejected(json.replace("Full declared purpose", "Full\u0000declared purpose"))
    }
    @Test fun wrongTypesCannotBeCoercedToTextOrProtocol() {
        for (value in listOf(json.replace("\"purpose\":\"Full declared purpose without truncation.\"", "\"purpose\":42"),
            json.replace("[\"fixture-worker\"]", "[42]"), json.replace("[1,2]", "[1.5]"), json.replace("[1,2]", "[\"1\"]"),
            json.replace("\"sdkApiRange\":\"^1.0.0\"", "\"sdkApiRange\":42"))) rejected(value)
    }
}
