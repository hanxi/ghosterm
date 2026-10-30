package com.hanxi.ghosterm.data

import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.Base64

data class IdentityPublicInfo(val algorithm: String?, val fingerprint: String?, val unavailable: String? = null)

/** Reads only the public section defined by OpenSSH PROTOCOL.key. Does not
 * decrypt/validate the private key or retain its bytes in strings or results.
 * https://github.com/openssh/openssh-portable/blob/master/PROTOCOL.key
 */
object IdentityPublicInfoReader {
    private val begin = "-----BEGIN OPENSSH PRIVATE KEY-----".toByteArray(Charsets.US_ASCII)
    private val end = "-----END OPENSSH PRIVATE KEY-----".toByteArray(Charsets.US_ASCII)
    private val magic = "openssh-key-v1\u0000".toByteArray(Charsets.US_ASCII)
    private fun offset(bytes: ByteArray, token: ByteArray, from: Int = 0): Int {
        for (i in from..bytes.size-token.size) if (token.indices.all { bytes[i+it] == token[it] }) return i
        return -1
    }
    private fun field(buffer: ByteBuffer): ByteArray {
        require(buffer.remaining() >= 4)
        val size = buffer.int
        require(size >= 0 && size <= buffer.remaining())
        return ByteArray(size).also(buffer::get)
    }
    private fun skip(buffer: ByteBuffer) {
        require(buffer.remaining() >= 4)
        val size = buffer.int
        require(size >= 0 && size <= buffer.remaining())
        buffer.position(buffer.position()+size)
    }
    fun read(material: ByteArray): IdentityPublicInfo {
        val start = offset(material,begin)
        if (start < 0) return IdentityPublicInfo(null,null,"此私钥格式暂不支持读取公钥指纹；仍可在连接时选择使用。")
        var encoded: ByteArray? = null
        var decoded: ByteArray? = null
        try {
            val finish = offset(material,end,start+begin.size)
            require(finish > start+begin.size)
            encoded = ByteArray(finish-start-begin.size)
            var count = 0
            for (i in start+begin.size until finish) {
                val value = material[i].toInt() and 255
                if (value != 9 && value != 10 && value != 13 && value != 32) encoded[count++] = material[i]
            }
            val compact = encoded.copyOf(count)
            try { decoded = Base64.getDecoder().decode(compact) } finally { compact.fill(0) }
            val buffer = ByteBuffer.wrap(decoded)
            require(buffer.remaining() >= magic.size && magic.indices.all { buffer.get() == magic[it] })
            repeat(3) { skip(buffer) } // cipher, KDF and KDF options
            require(buffer.remaining() >= 4 && buffer.int == 1)
            val publicKey = field(buffer)
            skip(buffer) // encrypted private section; never copy it
            require(!buffer.hasRemaining())
            val public = ByteBuffer.wrap(publicKey)
            val typeBytes = field(public)
            require(typeBytes.size in 1..64 && typeBytes.all { it.toInt() in 33..126 })
            val type = typeBytes.toString(Charsets.US_ASCII)
            when (type) {
                "ssh-ed25519" -> require(field(public).size == 32)
                "ssh-rsa" -> repeat(2) { require(field(public).isNotEmpty()) }
                "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521" -> {
                    val curve = type.removePrefix("ecdsa-sha2-")
                    require(field(public).toString(Charsets.US_ASCII) == curve)
                    val point = field(public)
                    val size = when(curve) { "nistp256" -> 65; "nistp384" -> 97; else -> 133 }
                    require(point.size == size && point[0] == 4.toByte())
                }
                else -> return IdentityPublicInfo(null,null,"此公钥类型暂不支持显示身份信息。")
            }
            require(!public.hasRemaining())
            val digest = MessageDigest.getInstance("SHA-256").digest(publicKey)
            return IdentityPublicInfo(type,"SHA256:"+Base64.getEncoder().withoutPadding().encodeToString(digest))
        } catch (_: Exception) {
            return IdentityPublicInfo(null,null,"密钥公开信息损坏，无法读取类型与指纹。")
        } finally { encoded?.fill(0); decoded?.fill(0) }
    }
}
