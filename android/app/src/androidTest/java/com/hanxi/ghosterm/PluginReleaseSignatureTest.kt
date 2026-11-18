package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ReleaseSignature
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.security.MessageDigest
import java.util.Base64

/** Node/OpenSSL-generated public vector, consumed by the actual JNI/Core backend. */
@RunWith(AndroidJUnit4::class)
class PluginReleaseSignatureTest {
    private val fixture by lazy {
        JSONObject(InstrumentationRegistry.getInstrumentation().context.assets
            .open("plugin-release/vector.json").bufferedReader().use { it.readText() })
    }
    private val descriptor get() = fixture.getString("descriptor").toByteArray()
    private val policy get() = fixture.getString("policy").toByteArray()
    private val signature get() = Base64.getDecoder().decode(fixture.getString("signature"))
    private val key get() = Base64.getDecoder().decode(JSONObject(String(policy)).getJSONArray("keys").getJSONObject(0).getString("publicKey"))
    private val identity = PluginReleaseSignature.Identity("org.ghosterm.signature", "1.0.0", "fixture", "a".repeat(64))
    private fun hash(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }
    private fun verify(d: ByteArray = descriptor, p: ByteArray = policy, s: ByteArray = signature,
        i: PluginReleaseSignature.Identity = identity) = PluginReleaseSignature.verify(d, s, p, i)
    private fun rejected(expected: PluginReleaseSignature.Failure, d: ByteArray = descriptor,
        p: ByteArray = policy, s: ByteArray = signature, i: PluginReleaseSignature.Identity = identity) {
        try { verify(d, p, s, i); fail("invalid release accepted: $expected") }
        catch (error: PluginReleaseSignature.Rejected) { assertEquals(expected, error.reason) }
    }
    // Deliberately bypass the public size guard to exercise the actual JNI guard.
    private fun nativeVerify(k: ByteArray, s: ByteArray, d: ByteArray): Int {
        val type = Class.forName("com.hanxi.ghosterm.bridge.NativeReleaseSignature")
        return type.getDeclaredMethod("verify", ByteArray::class.java, ByteArray::class.java, ByteArray::class.java)
            .invoke(type.getDeclaredField("INSTANCE").get(null), k, s, d) as Int
    }
    private fun changedPolicy(change: (JSONObject) -> Unit) = JSONObject(String(policy)).also(change).toString().toByteArray()
    private fun changedDescriptor(change: (JSONObject) -> Unit) = JSONObject(String(descriptor)).also(change).toString().toByteArray()

    @Test fun independentPublicVectorAndProofPinExactBytes() {
        assertEquals(0, ReleaseSignature.verify(key, signature, descriptor))
        val proof = verify()
        assertEquals(identity.id, proof.pluginID)
        assertEquals(identity.packageSHA256, proof.packageSHA256)
        assertEquals(hash(policy), proof.policySHA256)
        assertEquals(hash(descriptor), proof.descriptorSHA256)
        assertEquals("fixture", proof.channel)
        assertEquals("b".repeat(64), proof.artifactSHA256)
        assertEquals(22L, proof.artifactBytes)
    }
    @Test fun channelRootBindsRawPolicyBytesAndFreshness() {
        val channel = JSONObject(InstrumentationRegistry.getInstrumentation().context.assets
            .open("plugin-release/channel-vector.json").bufferedReader().use { it.readText() })
        val statement = channel.getString("statement").toByteArray()
        val root = Base64.getDecoder().decode(channel.getString("root"))
        val signed = Base64.getDecoder().decode(channel.getString("signature"))
        fun read(bytes: ByteArray = policy, trust: ByteArray = root, now: ULong = 150u) =
            PluginReleaseSignature.verifyChannelPolicy(statement, signed, bytes, "fixture", trust, now)
        fun reject(expected: PluginReleaseSignature.Failure, action: () -> Unit) {
            try { action(); fail("invalid channel accepted: $expected") }
            catch (e: PluginReleaseSignature.Rejected) { assertEquals(expected, e.reason) }
        }
        val accepted = read()
        assertEquals(2uL, accepted.generation)
        assertEquals(hash(policy), accepted.authenticate(descriptor, signature, 199u).proof.policySHA256)
        assertEquals(4, ReleaseSignature.verify(root, signed, statement))
        reject(PluginReleaseSignature.Failure.CHANNEL_STATEMENT) { read(policy + byteArrayOf(10)) }
        reject(PluginReleaseSignature.Failure.CHANNEL_STATEMENT) { read(trust = key) }
        reject(PluginReleaseSignature.Failure.CHANNEL_FRESHNESS) { read(now = 99u) }
        reject(PluginReleaseSignature.Failure.CHANNEL_FRESHNESS) { accepted.authenticate(descriptor, signature, 200u) }
        val directory = PluginInstallTestFiles.root()
        try {
            val store = PluginChannelPolicyStore(directory, "fixture", root)
            reject(PluginReleaseSignature.Failure.CHANNEL_HISTORY) { store.useCurrent(150u) { it.generation } }
            assertEquals(2uL, store.refresh(statement, signed, policy, 150u).generation)
            val reopened = PluginChannelPolicyStore(directory, "fixture", root)
            assertEquals(2uL, reopened.useCurrent(151u) {
                val busy = runCatching { store.refresh(statement, signed, policy, 151u) }.exceptionOrNull()
                assertEquals(PluginReleaseCatalog.Reason.BUSY, (busy as? PluginReleaseCatalog.Rejected)?.reason)
                it.generation
            })
            reject(PluginReleaseSignature.Failure.CHANNEL_HISTORY) { reopened.useCurrent(150u) { it.generation } }
            reject(PluginReleaseSignature.Failure.CHANNEL_FRESHNESS) { reopened.useCurrent(200u) { it.generation } }
            fun refresh(name: String, time: ULong): PluginReleaseSignature.VerifiedChannelPolicy {
                val candidate = channel.getJSONObject(name)
                return reopened.refresh(candidate.getString("statement").toByteArray(),
                    Base64.getDecoder().decode(candidate.getString("signature")), policy, time)
            }
            reject(PluginReleaseSignature.Failure.CHANNEL_HISTORY) { refresh("older", 152u) }
            reject(PluginReleaseSignature.Failure.CHANNEL_HISTORY) { refresh("fork", 152u) }
            assertEquals(2uL, reopened.useCurrent(152u) { it.generation })
            assertEquals(3uL, refresh("next", 201u).generation)
            assertEquals(3uL, store.useCurrent(202u) { it.generation })
        } finally { directory.deleteRecursively() }
    }

    @Test fun signatureAndRawBytesAreNotCanonicalized() {
        rejected(PluginReleaseSignature.Failure.SIGNATURE, s = signature.also { it[63] = (it[63].toInt() xor 1).toByte() })
        rejected(PluginReleaseSignature.Failure.SIGNATURE, d = descriptor + byteArrayOf(10))
        assertEquals(4, ReleaseSignature.verify(key, signature, descriptor.copyOfRange(1, descriptor.size)))
        assertEquals(4, ReleaseSignature.verify(key.also { it[0] = (it[0].toInt() xor 1).toByte() }, signature, descriptor))
    }
    @Test fun coreAndJniEnforceArgumentBounds() {
        assertEquals(1, ReleaseSignature.verify(ByteArray(31), signature, descriptor))
        assertEquals(1, ReleaseSignature.verify(key, ByteArray(63), descriptor))
        assertEquals(1, ReleaseSignature.verify(key, signature, ByteArray(0)))
        assertEquals(1, ReleaseSignature.verify(key, signature, ByteArray(65537)))
        assertEquals(1, nativeVerify(ByteArray(33), signature, descriptor))
        assertEquals(1, nativeVerify(key, ByteArray(65), descriptor))
        assertEquals(1, nativeVerify(key, signature, ByteArray(65537)))
        rejected(PluginReleaseSignature.Failure.LIMIT, s = ByteArray(65))
        rejected(PluginReleaseSignature.Failure.LIMIT, p = ByteArray(131073))
        rejected(PluginReleaseSignature.Failure.LIMIT, d = ByteArray(65537))
    }
    @Test fun policyScopesUnknownKeysAndRevocationsRemainDistinct() {
        val keyID = JSONObject(String(descriptor)).getString("keyId")
        rejected(PluginReleaseSignature.Failure.REVOKED_KEY, p = changedPolicy { it.getJSONArray("revokedKeyIds").put(keyID) })
        rejected(PluginReleaseSignature.Failure.UNKNOWN_KEY, d = changedDescriptor { it.put("keyId", "c".repeat(64)) })
        rejected(PluginReleaseSignature.Failure.SCOPE, p = changedPolicy { it.put("channel", "other") })
        rejected(PluginReleaseSignature.Failure.SCOPE, p = changedPolicy { it.getJSONArray("keys").getJSONObject(0).put("publisher", "other") })
        rejected(PluginReleaseSignature.Failure.SCOPE, p = changedPolicy { it.getJSONArray("keys").getJSONObject(0).getJSONArray("pluginIds").put(0, "org.ghosterm.other") })
    }
    @Test fun policyRejectsNoncanonicalAndMismatchedPublicKeys() {
        rejected(PluginReleaseSignature.Failure.POLICY, p = changedPolicy { val k = it.getJSONArray("keys").getJSONObject(0); k.put("publicKey", k.getString("publicKey").trimEnd('=')) })
        rejected(PluginReleaseSignature.Failure.POLICY, p = changedPolicy { it.getJSONArray("keys").getJSONObject(0).put("id", "c".repeat(64)) })
        rejected(PluginReleaseSignature.Failure.POLICY, p = changedPolicy { it.getJSONArray("keys").getJSONObject(0).put("algorithm", "rsa") })
        rejected(PluginReleaseSignature.Failure.POLICY, p = changedPolicy { it.getJSONArray("keys").put(it.getJSONArray("keys").getJSONObject(0)) })
        rejected(PluginReleaseSignature.Failure.POLICY, p = changedPolicy { it.put("extra", true) })
        rejected(PluginReleaseSignature.Failure.DESCRIPTOR, d = changedDescriptor { it.put("extra", true) })
    }
    @Test fun signatureBindsEveryReviewedPackageIdentityField() {
        for (other in listOf(identity.copy(id = "org.ghosterm.other"), identity.copy(version = "2.0"),
            identity.copy(publisher = "other"), identity.copy(packageSHA256 = "b".repeat(64)))) {
            rejected(PluginReleaseSignature.Failure.IDENTITY, i = other)
        }
    }
    @Test fun strictJsonRejectsBOMControlsMalformedUtf8AndSurrogates() {
        rejected(PluginReleaseSignature.Failure.JSON, p = byteArrayOf(0xef.toByte(), 0xbb.toByte(), 0xbf.toByte()) + policy)
        rejected(PluginReleaseSignature.Failure.JSON, p = String(policy).replace("fixture", "fix\nture").toByteArray())
        rejected(PluginReleaseSignature.Failure.JSON, p = String(policy).replace("fixture", "\\uD800").toByteArray())
        rejected(PluginReleaseSignature.Failure.JSON, p = byteArrayOf(0xc0.toByte(), 0xaf.toByte()))
        rejected(PluginReleaseSignature.Failure.JSON, p = (String(policy) + "{}").toByteArray())
        rejected(PluginReleaseSignature.Failure.JSON, p = String(policy).dropLast(1).plus(",}").toByteArray())
        rejected(PluginReleaseSignature.Failure.JSON, p = String(policy).replace("fixture", "\\x41").toByteArray())
    }
    @Test fun duplicateEscapedNormalizedAndPrototypeKeysAreRejected() {
        rejected(PluginReleaseSignature.Failure.DUPLICATE_KEY, p = String(policy).replaceFirst("{", "{\"chann\\u0065l\":\"fixture\",").toByteArray())
        rejected(PluginReleaseSignature.Failure.DUPLICATE_KEY, p = String(policy).replaceFirst("{", "{\"é\":1,\"e\\u0301\":2,").toByteArray())
        for (name in listOf("__proto__", "constructor", "prototype")) {
            rejected(PluginReleaseSignature.Failure.DUPLICATE_KEY, p = String(policy).replaceFirst("{", "{\"$name\":{},").toByteArray())
        }
    }
    @Test fun boundedTreeRejectsExcessDepthAndNodes() {
        rejected(PluginReleaseSignature.Failure.LIMIT, p = ("[".repeat(18) + "0" + "]".repeat(18)).toByteArray())
        rejected(PluginReleaseSignature.Failure.LIMIT, p = ("[" + List(8200) { "0" }.joinToString(",") + "]").toByteArray())
    }
    @Test fun exactNumericLexemesMatchIntegerSemantics() {
        for (one in listOf("1.0", "1e0", "10e-1", "0.1e1")) {
            assertEquals(identity.id, verify(p = String(policy).replace("\"schemaVersion\":1", "\"schemaVersion\":$one").toByteArray()).pluginID)
        }
        for (bad in listOf("true", "1.0000000000000000001", "1e999999999", "-1", "0", "1.5")) {
            rejected(PluginReleaseSignature.Failure.POLICY, p = String(policy).replace("\"schemaVersion\":1", "\"schemaVersion\":$bad").toByteArray())
        }
        for (number in listOf("22.0", "220e-1", "75497472")) {
            rejected(PluginReleaseSignature.Failure.SIGNATURE, d = String(descriptor).replace("\"bytes\":22", "\"bytes\":$number").toByteArray())
        }
        for (number in listOf("-0", "21", "75497473", "22.0000000000000000001", "9223372036854775808", "22e-1000000")) {
            rejected(PluginReleaseSignature.Failure.DESCRIPTOR, d = String(descriptor).replace("\"bytes\":22", "\"bytes\":$number").toByteArray())
        }
    }
    @Test fun uiThreadCannotRunCrypto() {
        InstrumentationRegistry.getInstrumentation().runOnMainSync {
            try { ReleaseSignature.verify(key, signature, descriptor); fail("crypto ran on main") }
            catch (expected: IllegalStateException) { assertTrue(expected.message!!.contains("worker")) }
        }
    }
}
