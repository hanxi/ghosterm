package com.hanxi.ghosterm

import android.app.Application
import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.nio.file.Path
import java.security.KeyStore
import java.util.Base64
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class IdentityVaultTest {
    private fun keys() = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
    private fun aliases() = keys().aliases().toList().filter { it.startsWith("ghosterm.ssh.identity.") }.toSet()
    private fun isolated(test: (File) -> Unit) {
        val directory = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir,"identity-${UUID.randomUUID()}")
        val before = aliases()
        try { test(directory) } finally {
            (aliases()-before).forEach { keys().deleteEntry(it) }
            directory.deleteRecursively()
        }
    }
    private fun material() = sshFixture().getString("pem").toByteArray()
    @Test fun defaultFactoryHasApplicationConstructor() {
        assertTrue("Production AndroidViewModel factory requires Application constructor",
            SessionsViewModel::class.java.constructors.any { it.parameterTypes.contentEquals(arrayOf(Application::class.java)) })
    }
    @Test fun encryptedRoundTripConsumesMaterialAndRemovesAccess() = isolated { dir ->
        val vault = IdentityVault(dir)
        val original = material(); val supplied = original.copyOf()
        try {
            val identity = vault.import("fixture",supplied)
            assertTrue("import caller buffer must be cleared",supplied.all { it == 0.toByte() })
            assertFalse(identity.needsPassphrase)
            assertEquals(listOf(identity),vault.list())
            assertEquals(0x1c0,Os.stat(dir.path).st_mode and 0x1ff)
            val file = File(dir,"${identity.id}.json")
            assertEquals(0x180,Os.stat(file.path).st_mode and 0x1ff)
            val envelope = JSONObject(file.readText())
            val ciphertext = Base64.getDecoder().decode(envelope.getString("ciphertext"))
            assertFalse("container must not store encoded plaintext",ciphertext.contentEquals(original))
            assertFalse("container must not contain PEM",file.readText().contains("PRIVATE KEY"))
            assertNull("Keystore wrapping key must not be exportable",keys().getKey("ghosterm.ssh.identity.${identity.id}",null).encoded)
            val unlocked = vault.unlock(identity)
            try { assertTrue("decrypted bytes differ",unlocked.contentEquals(original)) } finally { unlocked.fill(0) }
            vault.remove(identity)
            assertTrue(vault.list().isEmpty())
            assertFalse(keys().containsAlias("ghosterm.ssh.identity.${identity.id}"))
        } finally { original.fill(0); supplied.fill(0) }
    }
    @Test fun changedMetadataAndCiphertextCannotUnlock() = isolated { dir ->
        val vault = IdentityVault(dir); val identity = vault.import("fixture",material())
        val file = File(dir,"${identity.id}.json"); val original = file.readText()
        file.writeText(JSONObject(original).put("name","tampered").toString())
        val altered = vault.list().single()
        assertThrows(Exception::class.java) { vault.unlock(altered) }
        file.writeText(original)
        val json = JSONObject(original)
        val encrypted = Base64.getDecoder().decode(json.getString("ciphertext")); encrypted[0] = (encrypted[0].toInt() xor 1).toByte()
        file.writeText(json.put("ciphertext",Base64.getEncoder().encodeToString(encrypted)).toString())
        assertThrows(Exception::class.java) { vault.unlock(identity) }
    }
    @Test fun missingKeystoreKeyNeverRegeneratesOrFallsBack() = isolated { dir ->
        val vault = IdentityVault(dir); val identity = vault.import("fixture",material())
        val alias = "ghosterm.ssh.identity.${identity.id}"
        keys().deleteEntry(alias)
        val failure = assertThrows(IllegalStateException::class.java) { vault.unlock(identity) }
        assertEquals("安全密钥不可用，请重新导入身份",failure.message)
        assertFalse(keys().containsAlias(alias))
    }
    @Test fun invalidOrNulMaterialIsRejectedAndCleared() = isolated { dir ->
        val vault = IdentityVault(dir)
        val valid = material()
        try {
            for (bytes in listOf("not a private key".toByteArray(),valid + byteArrayOf(0,65,66))) {
                assertThrows(IllegalStateException::class.java) { vault.import("invalid",bytes) }
                assertTrue("invalid material not cleared",bytes.all { it == 0.toByte() })
            }
            assertFalse(dir.exists())
        } finally { valid.fill(0) }
    }
    @Test fun failedCommitRemovesNewKeyAndTemporaryContainer() = isolated { dir ->
        val before = aliases(); val bytes = material()
        val vault = IdentityVault(dir,object : ConfigFileOps() {
            override fun replace(from: Path,to: Path) { error("injected commit failure") }
        })
        assertThrows(IllegalStateException::class.java) { vault.import("fixture",bytes) }
        assertTrue(bytes.all { it == 0.toByte() })
        assertEquals(before,aliases()); assertTrue(dir.listFiles()!!.isEmpty())
    }
    @Test fun syncFailureReportsCommittedIdentityWithoutDeletingItsKey() = isolated { dir ->
        val vault = IdentityVault(dir,object : ConfigFileOps() {
            override fun syncDirectory(directory: Path) { error("injected sync failure") }
        })
        val original = material()
        try {
            val failure = assertThrows(IllegalStateException::class.java) { vault.import("fixture",original.copyOf()) }
            assertEquals("身份已保存，但目录同步失败，请刷新身份列表",failure.message)
            val identity = vault.list().single()
            assertTrue("committed wrapping key was removed",keys().containsAlias("ghosterm.ssh.identity.${identity.id}"))
            val unlocked = vault.unlock(identity)
            try { assertTrue("committed identity remains usable",unlocked.contentEquals(original)) } finally { unlocked.fill(0) }
        } finally { original.fill(0) }
    }
}
