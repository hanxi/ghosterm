package com.hanxi.ghosterm.data

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.system.Os
import com.hanxi.ghosterm.bridge.CoreLibrary
import org.json.JSONObject
import java.io.File
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.StandardOpenOption.READ
import java.nio.ByteBuffer
import java.security.KeyStore
import java.util.Base64
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

data class StoredIdentity(val id: String, val name: String, val needsPassphrase: Boolean)

/** Blocking private storage. AES key stays in AndroidKeyStore; SSH PEM is encrypted
 * at rest and only returned as a caller-owned, erasable buffer after explicit selection.
 * This is not a hardware SSH signer or biometric-per-use authorization.
 */
class IdentityVault(private val directory: File, private val ops: ConfigFileOps = ConfigFileOps()) {
    companion object { private val lock = Any() }
    private fun keyAlias(id: String) = "ghosterm.ssh.identity.$id"
    private fun file(id: String): File {
        require(UUID.fromString(id).toString() == id) { "身份引用无效" }
        return File(directory, "$id.json")
    }
    private fun keystore() = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
    private fun metadata(json: JSONObject, id: String): StoredIdentity {
        check(json.getInt("version") == 1 && json.getString("id") == id) { "身份容器格式无效" }
        return StoredIdentity(id,json.getString("name"),json.getBoolean("needsPassphrase"))
    }
    private fun aad(identity: StoredIdentity) = "ghosterm-identity-v1\n${identity.id}\n${identity.name}\n${identity.needsPassphrase}".toByteArray(Charsets.UTF_8)
    private fun read(id: String): JSONObject {
        check(!Files.isSymbolicLink(directory.toPath())) { "身份目录不能是符号链接" }
        val path = file(id).toPath()
        check(!Files.isSymbolicLink(path) && Files.isRegularFile(path,NOFOLLOW_LINKS)) { "身份文件不可读取，请重新导入" }
        val data = java.nio.channels.FileChannel.open(path,READ,NOFOLLOW_LINKS).use { channel ->
            check(channel.size() in 1..(2L*1024*1024)) { "身份文件大小无效" }
            val out = ByteBuffer.allocate(channel.size().toInt())
            while (out.hasRemaining()) check(channel.read(out) > 0) { "身份文件读取不完整" }
            out.array()
        }
        return JSONObject(data.toString(Charsets.UTF_8))
    }
    fun list(): List<StoredIdentity> = synchronized(lock) {
        if (!directory.exists()) return@synchronized emptyList()
        check(directory.isDirectory && !Files.isSymbolicLink(directory.toPath())) { "身份目录不可读取" }
        checkNotNull(directory.listFiles()) { "身份目录不可读取" }.filter { it.extension == "json" }
            .map { metadata(read(it.nameWithoutExtension),it.nameWithoutExtension) }.sortedBy { it.name }
    }
    /** Consumes and clears material even if format checking, encryption, or commit fails. */
    fun import(name: String, material: ByteArray): StoredIdentity = synchronized(lock) {
        var id: String? = null
        var committed = false
        var temporary: File? = null
        try {
            require(name.isNotBlank() && name.length <= 128 && name.none { it.isISOControl() }) { "密钥名称无效" }
            require(material.isNotEmpty() && material.size <= 1024*1024) { "密钥大小无效" }
            val requirement = CoreLibrary.keyPassphraseRequirement(material)
            check(requirement in 0..1) { "无法识别此密钥格式；不会猜测口令" }
            check(!Files.isSymbolicLink(directory.toPath())) { "身份目录不能是符号链接" }
            Files.createDirectories(directory.toPath()); Os.chmod(directory.path,0x1c0)
            id = UUID.randomUUID().toString()
            val identity = StoredIdentity(id,name.trim(),requirement == 1)
            val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore")
            generator.init(KeyGenParameterSpec.Builder(keyAlias(id),KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256).setRandomizedEncryptionRequired(true).build())
            val secret = generator.generateKey()
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.ENCRYPT_MODE,secret); cipher.updateAAD(aad(identity))
            val encrypted = cipher.doFinal(material)
            val envelope = JSONObject().put("version",1).put("id",id).put("name",identity.name)
                .put("needsPassphrase",identity.needsPassphrase).put("iv",Base64.getEncoder().encodeToString(cipher.iv))
                .put("ciphertext",Base64.getEncoder().encodeToString(encrypted)).toString().toByteArray(Charsets.UTF_8)
            temporary = File(directory,".$id.pending")
            ops.writeExclusive(temporary.toPath(),envelope)
            ops.replace(temporary.toPath(),file(id).toPath()); committed = true
            try { ops.syncDirectory(directory.toPath()) }
            catch (failure: Exception) { throw IllegalStateException("身份已保存，但目录同步失败，请刷新身份列表",failure) }
            identity
        } finally {
            material.fill(0)
            temporary?.delete()
            if (!committed && id != null) keystore().deleteEntry(keyAlias(id))
        }
    }
    fun unlock(identity: StoredIdentity): ByteArray = synchronized(lock) {
        val json = read(identity.id)
        check(metadata(json,identity.id) == identity) { "身份资料已变化，请重新选择" }
        val secret = keystore().getKey(keyAlias(identity.id),null) as? SecretKey
            ?: error("安全密钥不可用，请重新导入身份")
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE,secret,GCMParameterSpec(128,Base64.getDecoder().decode(json.getString("iv"))))
        cipher.updateAAD(aad(identity))
        cipher.doFinal(Base64.getDecoder().decode(json.getString("ciphertext")))
    }
    fun remove(identity: StoredIdentity) = synchronized(lock) {
        // Remove access first: a crash may leave unreadable ciphertext, never an unlocked key.
        keystore().deleteEntry(keyAlias(identity.id))
        Files.delete(file(identity.id).toPath()); ops.syncDirectory(directory.toPath())
    }
}
