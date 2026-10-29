package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.nio.file.Path
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class ConfigRepositoryTest {
    private fun fixture(test: (File) -> Unit) {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val directory = File(context.cacheDir, "config-test-${UUID.randomUUID()}").apply { mkdirs() }
        try { test(directory) } finally { directory.deleteRecursively() }
    }
    private val source = "# 注释 🔑\r\nHost demo\r\n\tHostName=example.test\r\n\tUser=alice\r\n\tPort=22\r\n  FutureOption yes\r\n\r\nHost *\r\n  ServerAliveInterval 20\r\n"
    @Test fun sparseEditAndBackupPreserveUnchangedBytes() = fixture { directory ->
        val file = File(directory, "config").apply { writeBytes(source.toByteArray()) }
        val repo = ConfigRepository(directory)
        val initial = repo.load()
        assertEquals(listOf("demo"), initial.hosts.map { it.alias })
        val result = repo.saveHost(initial, HostEdit("demo", "demo", "example.test", "bob", "22"))
        assertEquals("bob", result.snapshot.hosts.single().user)
        assertEquals(source.replace("\tUser=alice\r\n", "\tUser=bob\n"), file.readText())
        assertArrayEquals(source.toByteArray(), directory.listFiles()!!.single { it.name.startsWith("config.backup-") }.readBytes())
        assertNotEquals(initial.revision.identity, result.snapshot.revision.identity)
    }
    @Test fun backupFailureLeavesOriginal() = fixture { directory ->
        val file = File(directory, "config").apply { writeText(source) }
        val ops = object : ConfigFileOps() {
            override fun writeExclusive(path: Path, bytes: ByteArray) {
                if (path.fileName.toString().startsWith("config.backup-")) throw IOException("injected backup failure")
                super.writeExclusive(path, bytes)
            }
        }
        val repo = ConfigRepository(directory, ops)
        val snapshot = repo.load()
        assertThrows(ConfigAccess::class.java) { repo.save(snapshot, source.replace("alice", "bob").toByteArray()) }
        assertEquals(source, file.readText())
    }
    @Test fun backupNameCollisionDoesNotDeleteExistingFile() = fixture { directory ->
        File(directory, "config").writeText(source)
        var collided: Path? = null
        val ops = object : ConfigFileOps() {
            override fun writeExclusive(path: Path, bytes: ByteArray) {
                if (path.fileName.toString().startsWith("config.backup-")) {
                    Files.write(path, "existing backup".toByteArray())
                    collided = path
                }
                super.writeExclusive(path, bytes)
            }
        }
        val repo = ConfigRepository(directory, ops)
        assertThrows(ConfigAccess::class.java) { repo.save(repo.load(), source.replace("alice", "bob").toByteArray()) }
        assertTrue("collision file must survive", Files.exists(collided!!))
        assertEquals("existing backup", collided!!.toFile().readText())
        assertEquals(source, File(directory, "config").readText())
    }
    @Test fun replaceFailureNeverDeletesOriginal() = fixture { directory ->
        val file = File(directory, "config").apply { writeText(source) }
        val ops = object : ConfigFileOps() {
            override fun replace(from: Path, to: Path) {
                assertTrue("original exists at commit", Files.exists(to))
                assertEquals(source, to.toFile().readText())
                throw IOException("injected rename failure")
            }
        }
        val repo = ConfigRepository(directory, ops)
        assertThrows(ConfigAccess::class.java) { repo.save(repo.load(), source.replace("alice", "bob").toByteArray()) }
        assertEquals(source, file.readText())
        assertFalse(directory.listFiles()!!.any { it.name.endsWith(".new") })
    }
    @Test fun equalSizeExternalEditIsRejected() = fixture { directory ->
        val file = File(directory, "config").apply { writeText(source) }
        val repo = ConfigRepository(directory); val snapshot = repo.load(); val time = file.lastModified()
        val changed = source.replace("alice", "carol")
        file.writeText(changed); file.setLastModified(time)
        assertThrows(ConfigConflict::class.java) { repo.save(snapshot, source.replace("alice", "david").toByteArray()) }
        assertEquals(changed, file.readText())
    }
    @Test fun externalEditDuringBackupIsRejected() = fixture { directory ->
        val file = File(directory, "config").apply { writeText(source) }
        val changed = source.replace("alice", "carol")
        val ops = object : ConfigFileOps() {
            override fun writeExclusive(path: Path, bytes: ByteArray) {
                super.writeExclusive(path, bytes)
                if (path.fileName.toString().startsWith("config.backup-")) file.writeText(changed)
            }
        }
        val repo = ConfigRepository(directory, ops)
        assertThrows(ConfigConflict::class.java) { repo.save(repo.load(), source.replace("alice", "david").toByteArray()) }
        assertEquals(changed, file.readText())
    }
    @Test fun unspecifiedRemoteUserRemainsEmpty() = fixture { directory ->
        File(directory, "config").writeText("Host demo\n  HostName example.test\n")
        assertEquals("", ConfigRepository(directory).load().hosts.single().user)
    }
    @Test fun complexDuplicateEditCannotRemoveOtherDirectives() = fixture { directory ->
        val original = "Host demo\n  HostName example.test\n  User alice\n  FutureOption preserve\n  User carol\n"
        val file = File(directory, "config").apply { writeText(original) }
        val repo = ConfigRepository(directory)
        assertThrows(ConfigAccess::class.java) { repo.saveHost(repo.load(), HostEdit("demo", "demo", "example.test", "bob", "22")) }
        assertEquals(original, file.readText())
    }
    @Test fun privateKeyContainerIsNeverSavedAsConfiguration() = fixture { directory ->
        val file = File(directory, "config").apply { writeText(source) }
        val repo = ConfigRepository(directory)
        // A container marker only: no actual private key material enters fixtures.
        val header = listOf("-----BEGIN", "OPENSSH", "PRIVATE", "KEY-----").joinToString(" ")
        assertThrows(ConfigAccess::class.java) { repo.save(repo.load(), header.toByteArray()) }
        assertEquals(source, file.readText())
    }
    @Test fun firstSaveAndReloadDeriveHostsFromConfig() = fixture { directory ->
        val repo = ConfigRepository(directory); val empty = repo.load()
        assertTrue(empty.hosts.isEmpty())
        val saved = repo.saveHost(empty, HostEdit(null, "new-host", "192.0.2.4", "deploy", "2222"))
        assertEquals("deploy@192.0.2.4:2222", saved.snapshot.hosts.single().address)
        assertEquals(saved.snapshot.hosts, ConfigRepository(directory).load().hosts)
        assertFalse(directory.listFiles()!!.any { it.name.startsWith("config.backup-") })
        assertThrows(ConfigAccess::class.java) { repo.saveHost(saved.snapshot, HostEdit(null, "NEW-HOST", "192.0.2.5", "deploy", "22")) }
    }
    @Test fun inheritedValuesAreNotWrittenBack() = fixture { directory ->
        val file = File(directory, "config").apply { writeText("Host demo\n  HostName example.test\nHost *\n  User inherited\n  Port 2222\n") }
        val repo = ConfigRepository(directory); val initial = repo.load()
        repo.saveHost(initial, HostEdit("demo", "demo", "changed.test", "inherited", "2222"))
        assertEquals("Host demo\n  HostName changed.test\nHost *\n  User inherited\n  Port 2222\n", file.readText())
    }
    @Test fun missingIncludeAndInvalidUtf8RemainErrors() = fixture { directory ->
        val file = File(directory, "config")
        file.writeText("Include ${directory.path}/absent.conf\n")
        assertThrows(IllegalStateException::class.java) { ConfigRepository(directory).load() }
        file.writeBytes(byteArrayOf(0xc3.toByte(), 0x28))
        assertThrows(ConfigAccess::class.java) { ConfigRepository(directory).load() }
        file.delete(); file.mkdir()
        assertThrows(ConfigAccess::class.java) { ConfigRepository(directory).load() }
    }
    @Test fun includeUsesAppHomeAndOriginalDirectiveSurvives() = fixture { directory ->
        val home = File(android.system.Os.getenv("HOME")!!, ".ssh").apply { mkdirs() }
        val include = File(home, "test-${UUID.randomUUID()}.conf")
        include.writeText("Host included\n  HostName actual.test\n  User me\n")
        try {
            val text = "Include ~/.ssh/${include.name}\n"
            File(directory, "config").writeText(text)
            val snapshot = ConfigRepository(directory).load()
            assertEquals("actual.test", snapshot.hosts.single().hostname)
            assertEquals(text, snapshot.text)
        } finally { include.delete() }
    }
}
