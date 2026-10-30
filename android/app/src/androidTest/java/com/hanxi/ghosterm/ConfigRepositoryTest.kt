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
    @Test fun connectionAdmissionReportsSharedSourcesWithoutEditing() {
        val bytes = "Host web\n HostName 127.0.0.1\n ProxyJump gateway\nHost other\n ProxyJump none\n".toByteArray()
        val saved = bytes.copyOf()
        val issues = com.hanxi.ghosterm.bridge.ConfigBridge.connectionIssues(bytes, "web")
        assertEquals(listOf("proxyjump"), issues.map { it.keyword })
        assertEquals(3, issues.single().sourceLine)
        assertEquals("", issues.single().sourcePath)
        assertFalse(issues.single().included)
        assertTrue(com.hanxi.ghosterm.bridge.ConfigBridge.connectionIssues(bytes, "other").isEmpty())
        assertArrayEquals(saved, bytes)
    }

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
    @Test fun provenanceDistinguishesShadowedExplicitInheritedAndDefault() = fixture { directory ->
        File(directory, "config").writeText("User inherited\nHost demo sibling\n  HostName example.test\n  User explicit\nHost *\n  Port 2222\n")
        val host = ConfigRepository(directory).load().hosts.first()
        assertEquals("inherited", host.user)
        val source = host.source!!
        assertEquals("Host demo sibling\n", source.header)
        assertEquals(2, source.patternCount)
        assertTrue(source.editable)
        assertEquals("explicit", source.user.explicitValue)
        assertFalse(source.user.effectiveIsExplicit)
        assertEquals("继承自全局配置 · 第 1 行；此块设置的 explicit 未生效", source.user.description)
        assertEquals("此主机显式设置 · 第 3 行", source.hostname.description)
        assertEquals("继承自 Host * · 第 6 行", source.port.description)
        File(directory, "config").writeText("Host demo\n")
        val defaults = ConfigRepository(directory).load().hosts.single().source!!
        assertEquals("默认值（未显式设置）", defaults.port.description)
        assertNull(defaults.user.explicitValue)
    }
    @Test fun ambiguousAndIncludedHostsAreReadOnlyBeforeEditing() = fixture { directory ->
        val child = File(directory, "included.conf").apply { writeText("Host remote\n  HostName remote.test\n  User me\n") }
        val text = "Include ${child.path}\nHost dup\n  User first\nHost DUP\n  User second\n"
        val file = File(directory, "config").apply { writeText(text) }
        var edits = 0
        val repo = ConfigRepository(directory, editConfig = { bytes, _, _, _ -> edits++; bytes })
        val snapshot = repo.load()
        assertEquals(listOf("remote", "dup"), snapshot.hosts.map { it.alias })
        val remote = snapshot.hosts.first().source!!
        assertTrue(remote.included)
        assertEquals(child.path, remote.path)
        assertEquals(1, remote.line)
        assertEquals(child.path, remote.user.path)
        assertEquals(3, remote.user.line)
        assertFalse(remote.editable)
        assertEquals(2, snapshot.hosts.last().source!!.matchingBlocks)
        assertFalse(snapshot.hosts.last().source!!.editable)
        for (alias in listOf("remote", "dup")) {
            assertThrows(ConfigAccess::class.java) { repo.saveHost(snapshot, HostEdit(alias, alias, "changed.test", "me", "22")) }
        }
        assertEquals("read-only must be rejected before calling editor", 0, edits)
        assertEquals(text, file.readText())
        assertFalse(directory.listFiles()!!.any { it.name.startsWith("config.backup-") })
    }
    @Test fun complexDuplicateEditPreservesOtherDirectives() = fixture { directory ->
        val original = "Host demo\n  HostName example.test\n  User alice\n  FutureOption preserve\n  User carol\n"
        val file = File(directory, "config").apply { writeText(original) }
        val repo = ConfigRepository(directory)
        val initial = repo.load()
        val saved = runCatching { repo.saveHost(initial, HostEdit.from(initial.hosts.single()).copy(user = "bob")) }
        assertTrue("Sparse duplicate edit must succeed: ${saved.exceptionOrNull()}", saved.isSuccess)
        assertEquals("Host demo\n  HostName example.test\n  User bob\n  FutureOption preserve\n", file.readText())
        assertEquals("bob", saved.getOrThrow().snapshot.hosts.single().user)
        assertArrayEquals(original.toByteArray(), directory.listFiles()!!.single { it.name.startsWith("config.backup-") }.readBytes())
    }
    @Test fun unrelatedLineChangeIsRejected() = fixture { directory ->
        val original = "Host demo\n  HostName example.test\n  User alice\n  FutureOption preserve\n".toByteArray()
        val corrupted = "Host demo\n  HostName example.test\n  User bob\n".toByteArray()
        val file = File(directory, "config").apply { writeBytes(original) }
        val repo = ConfigRepository(directory, editConfig = { _, _, _, _ -> corrupted })
        assertThrows(ConfigAccess::class.java) {
            val initial = repo.load()
            repo.saveHost(initial, HostEdit.from(initial.hosts.single()).copy(user = "bob"))
        }
        assertArrayEquals(original, file.readBytes())
        assertFalse(directory.listFiles()!!.any { it.name.startsWith("config.backup-") })
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
        repo.saveHost(initial, HostEdit.from(initial.hosts.single()).copy(hostname = "changed.test"))
        assertEquals("Host demo\n  HostName changed.test\nHost *\n  User inherited\n  Port 2222\n", file.readText())
    }
    @Test fun sameValueCanBeExplicitAndRestoredToInheritance() = fixture { directory ->
        val original = "Host demo\n  HostName example.test\nHost *\n  User inherited\n  Port 2222\n"
        val file = File(directory, "config").apply { writeText(original) }
        val repo = ConfigRepository(directory); val initial = repo.load()
        val override = HostEdit.from(initial.hosts.single()).withInheritance(HostField.USER, false)
        val explicit = repo.saveHost(initial, override).snapshot
        assertEquals("inherited", explicit.hosts.single().source!!.user.explicitValue)
        assertTrue(explicit.hosts.single().source!!.user.effectiveIsExplicit)
        assertNotEquals(original, file.readText())
        val restore = HostEdit.from(explicit.hosts.single()).withInheritance(HostField.USER, true)
        val saved = runCatching { repo.saveHost(explicit, restore) }
        assertTrue("Restoring inheritance must succeed: ${saved.exceptionOrNull()}", saved.isSuccess)
        assertEquals(original, file.readText())
        assertNull(saved.getOrThrow().snapshot.hosts.single().source!!.user.explicitValue)
        assertEquals("inherited", saved.getOrThrow().snapshot.hosts.single().user)
    }
    @Test fun emptyInheritedUserCanBeKeptAndRestored() = fixture { directory ->
        val file = File(directory, "config").apply { writeText("Host demo\n  HostName example.test\n") }
        val repo = ConfigRepository(directory); val initial = repo.load()
        val changed = runCatching { repo.saveHost(initial, HostEdit.from(initial.hosts.single()).copy(hostname = "new.test")) }
        assertTrue("Empty inherited User is valid: ${changed.exceptionOrNull()}", changed.isSuccess)
        assertEquals("", changed.getOrThrow().snapshot.hosts.single().user)
        val updated = changed.getOrThrow().snapshot
        val explicit = repo.saveHost(updated, HostEdit.from(updated.hosts.single())
            .withInheritance(HostField.USER, false).copy(user = "alice")).snapshot
        val restored = runCatching { repo.saveHost(explicit, HostEdit.from(explicit.hosts.single()).withInheritance(HostField.USER, true)) }
        assertTrue("Deleting the last User is valid: ${restored.exceptionOrNull()}", restored.isSuccess)
        assertEquals("", restored.getOrThrow().snapshot.hosts.single().user)
        assertEquals("Host demo\n  HostName new.test\n", file.readText())
    }
    @Test fun newHostMayInheritEveryConnectionField() = fixture { directory ->
        val repo = ConfigRepository(directory)
        val saved = repo.saveHost(repo.load(), HostEdit.from(null).copy(alias = "inherited-only",
            inherited = HostField.entries.toSet())).snapshot
        assertEquals(listOf("inherited-only"), saved.hosts.map { it.alias })
        assertEquals("inherited-only", saved.hosts.single().hostname)
        assertEquals("", saved.hosts.single().user)
        assertEquals(22, saved.hosts.single().port)
        assertEquals("Host inherited-only", File(directory, "config").readText().trim())
        for (field in HostField.entries) assertNull(field.source(saved.hosts.single())!!.explicitValue)
    }
    @Test fun shadowedOverrideIsRejectedBeforeBackup() = fixture { directory ->
        val original = "User earlier\nHost demo\n  HostName example.test\n"
        val file = File(directory, "config").apply { writeText(original) }
        val repo = ConfigRepository(directory); val initial = repo.load()
        assertThrows(ConfigAccess::class.java) {
            repo.saveHost(initial, HostEdit.from(initial.hosts.single()).withInheritance(HostField.USER, false).copy(user = "later"))
        }
        assertEquals(original, file.readText())
        assertFalse(directory.listFiles()!!.any { it.name.startsWith("config.backup-") })
    }
    @Test fun unchangedShadowedExplicitValueIsNotRewritten() = fixture { directory ->
        val original = "User inherited\nHost demo\n  User explicit\n  HostName example.test\n"
        val file = File(directory, "config").apply { writeText(original) }
        val repo = ConfigRepository(directory); val initial = repo.load()
        val saved = runCatching { repo.saveHost(initial, HostEdit.from(initial.hosts.single())) }
        assertTrue("Unchanged shadowed value must be preserved: ${saved.exceptionOrNull()}", saved.isSuccess)
        assertEquals(original, file.readText())
        assertFalse(directory.listFiles()!!.any { it.name.startsWith("config.backup-") })
    }
    @Test fun editorMustActuallyRemoveRequestedDirective() = fixture { directory ->
        val original = "Host demo\n  User explicit\n"
        val file = File(directory, "config").apply { writeText(original) }
        val repo = ConfigRepository(directory, editConfig = { bytes, _, _, _ -> bytes })
        val initial = repo.load()
        assertThrows(ConfigAccess::class.java) {
            repo.saveHost(initial, HostEdit.from(initial.hosts.single()).withInheritance(HostField.USER, true))
        }
        assertEquals(original, file.readText())
        assertFalse(directory.listFiles()!!.any { it.name.startsWith("config.backup-") })
    }
    @Test fun renamePreservesSiblingAndCanEditFieldsInSameSave() = fixture { directory ->
        val original = "# 注释 🔑\r\n\tHoSt=  \"demo\"\tsibling # shared\r\n  HostName example.test\r\n  User alice\r\n  FutureOption keep\r\n"
        val file = File(directory, "config").apply { writeText(original) }
        val repo = ConfigRepository(directory); val initial = repo.load()
        val saved = runCatching { repo.saveHost(initial, HostEdit.from(initial.hosts.first()).copy(alias = "renamed", user = "bob")) }
        assertTrue("Rename plus sparse edit must succeed: ${saved.exceptionOrNull()}", saved.isSuccess)
        assertEquals(listOf("renamed", "sibling"), saved.getOrThrow().snapshot.hosts.map { it.alias })
        assertTrue(saved.getOrThrow().snapshot.hosts.all { it.user == "bob" && it.hostname == "example.test" })
        assertEquals(original.replace("\"demo\"", "renamed").replace("  User alice\r\n", "  User bob\n"), file.readText())
        assertEquals(original, directory.listFiles()!!.single { it.name.startsWith("config.backup-") }.readText())
        assertEquals(listOf("renamed", "sibling"), repo.load().hosts.map { it.alias })
    }
    @Test fun renameOnlyAndCaseChangeKeepBodyBytes() = fixture { directory ->
        val original = "Host demo sibling\n  HostName explicit.test\n  User me\n  FutureOption value\n"
        val file = File(directory, "config").apply { writeText(original) }
        val repo = ConfigRepository(directory); val initial = repo.load()
        val renamed = runCatching { repo.saveHost(initial, HostEdit.from(initial.hosts.first()).copy(alias = "new-host")) }
        assertTrue("Rename-only must not be treated as a no-op: ${renamed.exceptionOrNull()}", renamed.isSuccess)
        assertEquals(original.replace("Host demo", "Host new-host"), file.readText())
        val state = renamed.getOrThrow().snapshot
        val cased = repo.saveHost(state, HostEdit.from(state.hosts.first()).copy(alias = "NEW-HOST")).snapshot
        assertEquals("NEW-HOST", cased.hosts.first().alias)
        assertEquals(original.replace("Host demo", "Host NEW-HOST"), file.readText())
    }
    @Test fun renameCollisionAndCorruptOutputNeverCommit() = fixture { directory ->
        val original = "Host demo sibling\n  User me\nHost occupied\n  Port 2200\n"
        val file = File(directory, "config").apply { writeText(original) }
        val repo = ConfigRepository(directory); val initial = repo.load()
        for (alias in listOf("SIBLING", "OCCUPIED")) {
            assertThrows(IllegalStateException::class.java) { repo.saveHost(initial, HostEdit.from(initial.hosts.first()).copy(alias = alias)) }
            assertEquals(original, file.readText())
        }
        val broken = ConfigRepository(directory, renameConfig = { _, _, _ -> original.replace("demo", "new-host").replace("Port 2200", "Port 9999").toByteArray() })
        assertThrows(ConfigAccess::class.java) { broken.saveHost(initial, HostEdit.from(initial.hosts.first()).copy(alias = "new-host")) }
        assertEquals(original, file.readText())
        assertFalse(directory.listFiles()!!.any { it.name.startsWith("config.backup-") })
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
