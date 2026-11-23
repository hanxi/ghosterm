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
    private val json = """{"schemaVersion":2,"sdkApiRange":"^1.0.0","bridgeProtocolRange":[1,2],"executor":{"entry":"executor.lua","requires":["session.rpc","process.spawn","process.pipes"],"programs":["fixture-worker"],"rpc":["fixture.run"]}}"""
    private fun metadata(bytes: ByteArray) = ExtensionPackage.Metadata("org.ghosterm.fixture", "1.0.0", "fixture", "MIT", "main.lua", "lua", "", "1.0.0",
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
        assertEquals(listOf("session.rpc", "process.spawn", "process.pipes"), result.permissions.map { it.capability })
        val permission = result.permissions.single { it.capability == "process.spawn" }
        assertEquals("executor", permission.side)
        assertEquals(listOf("fixture-worker"), permission.programs)
        assertNotNull("Executor declarations were discarded", result.executorJSON)
        val executor = JSONObject(result.executorJSON!!)
        assertEquals("executor.lua", executor.getString("entry"))
        assertEquals("process.spawn", executor.getJSONArray("requires").getString(1))
        assertEquals("fixture-worker", executor.getJSONArray("programs").getString(0))
        assertEquals("fixture.run", executor.getJSONArray("rpc").getString(0))
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
        rejected("\uFEFF$json"); rejected(json.replace("fixture-worker", "fixture\u0000worker"))
    }
    @Test fun wrongTypesCannotBeCoercedToTextOrProtocol() {
        for (value in listOf(json.replace("\"schemaVersion\":2", "\"schemaVersion\":1"),
            json.replace("\"schemaVersion\":2,", ""), json.replace("\"process.spawn\"", "42"),
            json.replace("[\"fixture-worker\"]", "[42]"), json.replace("[1,2]", "[1.5]"), json.replace("[1,2]", "[\"1\"]"),
            json.replace("\"sdkApiRange\":\"^1.0.0\"", "\"sdkApiRange\":42"))) rejected(value)
    }
}
