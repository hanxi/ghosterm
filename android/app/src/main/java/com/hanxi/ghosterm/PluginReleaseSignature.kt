package com.hanxi.ghosterm

import android.util.JsonReader
import android.util.JsonToken
import com.hanxi.ghosterm.bridge.ReleaseSignature
import java.io.StringReader
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest
import java.text.Normalizer
import java.util.Base64

/** Explicit policy bytes only; authentication is separate from source/Session consent. */
internal object PluginReleaseSignature {
    enum class Failure { JSON, DUPLICATE_KEY, LIMIT, POLICY, DESCRIPTOR, UNKNOWN_KEY, REVOKED_KEY, SCOPE, SIGNATURE, IDENTITY, UNSUPPORTED, CRYPTO_UNKNOWN }
    class Rejected(val reason: Failure) : IllegalArgumentException(when (reason) {
        Failure.REVOKED_KEY -> "此签名密钥已被所选策略撤销。"
        Failure.UNKNOWN_KEY -> "所选策略不认识此签名密钥。"
        Failure.SCOPE -> "渠道、发布者或插件不在所选策略的授权范围内。"
        Failure.SIGNATURE -> "发布签名不匹配。"
        Failure.IDENTITY -> "签名描述与已检查的固定包身份不一致。"
        Failure.UNSUPPORTED -> "当前构建不支持发布签名验证。"
        Failure.CRYPTO_UNKNOWN -> "发布签名验证未完成，不能判断为有效或签名不匹配。"
        Failure.LIMIT -> "发布签名材料超过读取或结构上限。"
        else -> "发布描述或渠道策略格式无效。"
    })
    data class Identity(val id: String, val version: String, val publisher: String, val packageSHA256: String)
    data class Proof(val pluginID: String, val packageSHA256: String, val channel: String, val keyID: String,
        val publisher: String, val policySHA256: String, val descriptorSHA256: String,
        val artifactSHA256: String, val artifactBytes: Long)
    data class Authenticated(val identity: Identity, val proof: Proof)

    /** Bounded strict JSON for display of a raw manifest already attested by Core. */
    internal fun declarationObject(bytes: ByteArray): org.json.JSONObject {
        parse(bytes).obj()
        return org.json.JSONObject(String(bytes, Charsets.UTF_8))
    }

    fun verify(descriptor: ByteArray, signature: ByteArray, trustedPolicy: ByteArray, identity: Identity): Proof {
        val authenticated = authenticate(descriptor, signature, trustedPolicy)
        ensure(authenticated.identity == identity, Failure.IDENTITY)
        return authenticated.proof
    }

    /** Authenticate before opening an archive; Core identity is bound after capture. */
    fun authenticate(descriptor: ByteArray, signature: ByteArray, trustedPolicy: ByteArray): Authenticated {
        ensure(descriptor.size <= 65536 && trustedPolicy.size <= 131072 && signature.size == 64, Failure.LIMIT)
        val payload = descriptor.copyOf(); val policyBytes = trustedPolicy.copyOf(); val signed = signature.copyOf()
        val policy = parse(policyBytes).obj()
        ensure(policy.keys == setOf("schemaVersion", "channel", "keys", "revokedKeyIds") && policy["schemaVersion"]?.integer(1) == 1L, Failure.POLICY)
        val channel = policy.getValue("channel").str(); ensure(channel.matches(Regex("[a-z0-9][a-z0-9.-]{0,63}")), Failure.POLICY)
        val entries = policy.getValue("keys").array(); ensure(entries.size in 1..64, Failure.POLICY)
        data class Key(val bytes: ByteArray, val publisher: String, val plugins: Set<String>)
        val keys = mutableMapOf<String, Key>()
        for (value in entries) {
            val entry = value.obj()
            ensure(entry.keys == setOf("id", "algorithm", "publicKey", "publisher", "pluginIds") && entry.getValue("algorithm").str() == "ed25519", Failure.POLICY)
            val id = entry.getValue("id").str(); ensure(digest(id) && id !in keys, Failure.POLICY)
            val encoded = entry.getValue("publicKey").str()
            val raw = try { Base64.getDecoder().decode(encoded) } catch (_: IllegalArgumentException) { reject(Failure.POLICY) }
            ensure(raw.size == 32 && Base64.getEncoder().encodeToString(raw) == encoded && hash(raw) == id, Failure.POLICY)
            val publisher = entry.getValue("publisher").str(); ensure(text(publisher), Failure.POLICY)
            val ids = entry.getValue("pluginIds").array().map { it.str() }
            ensure(ids.size in 1..64 && ids.toSet().size == ids.size && ids.all(::pluginID), Failure.POLICY)
            keys[id] = Key(raw, publisher, ids.toSet())
        }
        val revoked = policy.getValue("revokedKeyIds").array().map { it.str() }
        ensure(revoked.size <= 256 && revoked.toSet().size == revoked.size && revoked.all(::digest), Failure.POLICY)
        val release = parse(payload).obj()
        ensure(release.keys == setOf("schemaVersion", "channel", "keyId", "pluginId", "pluginVersion", "publisher", "packageSHA256", "artifact") && release["schemaVersion"]?.integer(1) == 1L, Failure.DESCRIPTOR)
        val requestedChannel = release.getValue("channel").str(); ensure(requestedChannel.matches(Regex("[a-z0-9][a-z0-9.-]{0,63}")), Failure.DESCRIPTOR)
        val keyID = release.getValue("keyId").str(); val id = release.getValue("pluginId").str()
        val version = release.getValue("pluginVersion").str(); val publisher = release.getValue("publisher").str(); val packageHash = release.getValue("packageSHA256").str()
        ensure(digest(keyID) && pluginID(id) && text(version) && text(publisher) && digest(packageHash), Failure.DESCRIPTOR)
        val artifact = release.getValue("artifact").obj()
        ensure(artifact.keys == setOf("format", "bytes", "sha256") && artifact.getValue("format").str() == "zip-v1", Failure.DESCRIPTOR)
        val count = artifact["bytes"]?.integer(75497472) ?: reject(Failure.DESCRIPTOR)
        val artifactHash = artifact.getValue("sha256").str(); ensure(count >= 22 && digest(artifactHash), Failure.DESCRIPTOR)
        ensure(requestedChannel == channel, Failure.SCOPE)
        val key = keys[keyID] ?: reject(Failure.UNKNOWN_KEY)
        ensure(keyID !in revoked, Failure.REVOKED_KEY)
        ensure(publisher == key.publisher && id in key.plugins, Failure.SCOPE)
        when (ReleaseSignature.verify(key.bytes, signed, payload)) {
            0 -> Unit
            4 -> reject(Failure.SIGNATURE)
            7 -> reject(Failure.UNSUPPORTED)
            else -> reject(Failure.CRYPTO_UNKNOWN)
        }
        return Authenticated(Identity(id, version, publisher, packageHash),
            Proof(id, packageHash, channel, keyID, publisher, hash(policyBytes), hash(payload), artifactHash, count))
    }
    private fun digest(value: String) = value.matches(Regex("[0-9a-f]{64}"))
    private fun pluginID(value: String) = value.toByteArray(Charsets.UTF_8).size <= 128 && value.matches(Regex("[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+"))
    private fun text(value: String) = value.codePointCount(0, value.length) in 1..128 &&
        Normalizer.normalize(value, Normalizer.Form.NFC) == value && value.codePoints().allMatch { it >= 32 && it != 127 }
    private fun hash(bytes: ByteArray): String {
        val hex = "0123456789abcdef"
        return buildString { for (byte in MessageDigest.getInstance("SHA-256").digest(bytes)) { val v = byte.toInt() and 255; append(hex[v shr 4]); append(hex[v and 15]) } }
    }
    private fun reject(reason: Failure): Nothing = throw Rejected(reason)
    private fun ensure(value: Boolean, reason: Failure) { if (!value) reject(reason) }

    // Android JsonReader accepts some non-JSON string controls and a leading
    // BOM even in strict mode. Enforce the JSON lexical rules before reading
    // the bounded tree; structural and number rules remain with the reader.
    private fun checkLexemes(source: String) {
        var quoted = false
        var index = 0
        while (index < source.length) {
            val character = source[index++]
            if (character == '"') { quoted = !quoted; continue }
            if (!quoted) {
                ensure(character in "{}[]:,-+0123456789.eEtruefalsnul \t\r\n", Failure.JSON)
            } else {
                ensure(character >= ' ', Failure.JSON)
                if (character == '\\') {
                    ensure(index < source.length, Failure.JSON)
                    val escaped = source[index++]
                    ensure(escaped in "\"\\/bfnrtu", Failure.JSON)
                    if (escaped == 'u') {
                        ensure(index + 4 <= source.length && source.substring(index, index + 4).all {
                            it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F'
                        }, Failure.JSON)
                        index += 4
                    }
                }
            }
        }
        ensure(!quoted, Failure.JSON)
    }

    private sealed interface Value {
        data class Object(val value: Map<String, Value>) : Value
        data class Array(val value: List<Value>) : Value
        data class StringValue(val value: String) : Value
        data class Number(val raw: String) : Value
        data class BooleanValue(val value: Boolean) : Value
        data object Null : Value
        fun obj() = (this as? Object)?.value ?: reject(Failure.JSON)
        fun array() = (this as? Array)?.value ?: reject(Failure.JSON)
        fun str() = (this as? StringValue)?.value ?: reject(Failure.JSON)
        fun integer(maximum: Long): Long? {
            val raw = (this as? Number)?.raw?.lowercase() ?: return null
            val negative = raw.startsWith('-'); val unsigned = raw.removePrefix("-")
            val parts = unsigned.split('e'); val exponent = if (parts.size == 2) parts[1].toIntOrNull() ?: return null else 0
            if (exponent !in -1000000..1000000) return null
            val decimal = parts[0].split('.'); var digits = decimal.joinToString("").trimStart('0')
            if (digits.isEmpty()) return 0
            if (negative) return null
            val scale = exponent - if (decimal.size == 2) decimal[1].length else 0
            if (scale < 0) {
                if (-scale > digits.length || digits.takeLast(-scale).any { it != '0' }) return null
                digits = digits.dropLast(-scale)
            } else {
                if (digits.length.toLong() + scale > 20) return null
                digits += "0".repeat(scale)
            }
            if (digits.length > 20) return null
            var result = 0L
            for (digit in digits) {
                if (result > maximum / 10) return null
                result = result * 10 + (digit - '0'); if (result > maximum) return null
            }
            return result
        }
    }
    private fun parse(bytes: ByteArray): Value {
        ensure(bytes.size <= 131072, Failure.LIMIT)
        val source = try { Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString() }
            catch (_: java.nio.charset.CharacterCodingException) { reject(Failure.JSON) }
        checkLexemes(source)
        var nodes = 0
        val number = Regex("-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?")
        fun validString(value: String): String {
            try { Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value)) }
            catch (_: java.nio.charset.CharacterCodingException) { reject(Failure.JSON) }
            return value
        }
        try {
            JsonReader(StringReader(source)).use { reader ->
                reader.isLenient = false
                fun read(depth: Int): Value {
                    nodes++; ensure(nodes <= 8192 && depth <= 16, Failure.LIMIT)
                    return when (reader.peek()) {
                        JsonToken.BEGIN_OBJECT -> {
                            val values = linkedMapOf<String, Value>(); val normalized = mutableSetOf<String>()
                            reader.beginObject()
                            while (reader.hasNext()) {
                                val key = validString(reader.nextName())
                                ensure(normalized.add(Normalizer.normalize(key, Normalizer.Form.NFC)) && key !in setOf("__proto__", "constructor", "prototype"), Failure.DUPLICATE_KEY)
                                values[key] = read(depth + 1)
                            }
                            reader.endObject(); Value.Object(values)
                        }
                        JsonToken.BEGIN_ARRAY -> {
                            val values = mutableListOf<Value>(); reader.beginArray()
                            while (reader.hasNext()) values += read(depth + 1)
                            reader.endArray(); Value.Array(values)
                        }
                        JsonToken.STRING -> Value.StringValue(validString(reader.nextString()))
                        JsonToken.NUMBER -> Value.Number(reader.nextString().also { ensure(number.matches(it), Failure.JSON) })
                        JsonToken.BOOLEAN -> Value.BooleanValue(reader.nextBoolean())
                        JsonToken.NULL -> { reader.nextNull(); Value.Null }
                        else -> reject(Failure.JSON)
                    }
                }
                val value = read(0); ensure(reader.peek() == JsonToken.END_DOCUMENT, Failure.JSON); return value
            }
        } catch (error: Rejected) { throw error }
        catch (_: java.io.IOException) { reject(Failure.JSON) }
        catch (_: IllegalStateException) { reject(Failure.JSON) }
    }
}
