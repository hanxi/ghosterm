package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ConfigBridge
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class ConfigIdentityTest {
    @Test fun identitySourcesUseCoreOrderingAndOwnership() {
        val bytes = ("IdentityFile ~/.ssh/global\r\nHost demo sibling\r\n" +
            "\tIdentityFile=\"/keys/中文 🔑 #1\"\r\n # preserve\r\n IdentityFile next\r\n" +
            "Host * !other\r\n IdentityFile inherited\r\nHost unrelated\r\n IdentityFile excluded\r\n").toByteArray()
        val original = bytes.copyOf()
        val entries = ConfigBridge.identities(bytes,"demo")
        assertEquals(listOf("~/.ssh/global","/keys/中文 🔑 #1","next","inherited"),entries.map { it.value })
        assertEquals(listOf(false,true,true,false),entries.map { it.explicitValue })
        assertEquals(listOf(1,3,5,7),entries.map { it.sourceLine })
        assertEquals(listOf("","Host demo sibling\r\n","Host demo sibling\r\n","Host * !other\r\n"),entries.map { it.sourceHeader })
        assertTrue(entries.all { it.effective && !it.included && it.sourcePath.isEmpty() })
        assertEquals(entries,ConfigBridge.identities(bytes,"sibling"))
        assertArrayEquals(original,bytes)
        val negated = ConfigBridge.identities("Host demo !demo\n IdentityFile own\nHost *\n IdentityFile wildcard\n".toByteArray(),"demo")
        assertTrue(negated[0].explicitValue)
        assertFalse(negated[0].effective)
        assertFalse(negated[1].explicitValue)
        assertTrue(negated[1].effective)
    }

    @Test fun includeSourcesRemainReadOnlyAndValuesAreCopied() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val directory = File(context.cacheDir,"identity-config-${UUID.randomUUID()}").apply { mkdirs() }
        try {
            val child = File(directory,"child.conf").apply { writeText("# child\n IdentityFile \"/keys/包含 🔑\"\n") }
            val bytes = "Host demo sibling\n Include ${child.path}\n IdentityFile local\n".toByteArray()
            val entries = ConfigBridge.identities(bytes,"demo")
            assertEquals(2,entries.size)
            assertTrue(entries[0].included)
            assertFalse(entries[0].explicitValue)
            assertTrue(entries[0].effective)
            assertEquals(child.path,entries[0].sourcePath)
            assertEquals(2,entries[0].sourceLine)
            assertEquals("/keys/包含 🔑",entries[0].value)
            assertTrue(entries[1].explicitValue)
            child.writeText("IdentityFile changed\n")
            assertEquals("changed",ConfigBridge.identities(bytes,"demo")[0].value)
            assertEquals("/keys/包含 🔑",entries[0].value)
        } finally { directory.deleteRecursively() }
    }

    @Test fun unsupportedAndMissingSourcesNeverBecomeEmptySuccess() {
        fun rejected(text: String,alias: String = "demo") {
            try { ConfigBridge.identities(text.toByteArray(),alias); fail("Expected explicit source error") }
            catch (_: IllegalStateException) { }
        }
        rejected("Host demo\n IdentityFile valid\nHost other\n Match all\n IdentityFile conditional\n")
        rejected("Host demo\n IdentityFile first second\n")
        rejected("Host demo\n IdentityFile\n")
        rejected("Host demo\n","missing")
        rejected("Host demo\n","demo\u0000other")
        assertTrue(ConfigBridge.identities("Host demo\n".toByteArray(),"demo").isEmpty())
        rejected("Host demo\n" + " IdentityFile key\n".repeat(4097))
    }
}
