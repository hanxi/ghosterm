package com.hanxi.ghosterm

import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/** The owned headless harness creates actual links: Android 10 SELinux denies
 * link creation to the app UID. This test never relaxes that system policy. */
@RunWith(AndroidJUnit4::class)
class PluginRemovalHardlinkTest {
    @Test fun realHardLinkedFixtureCannotBeListedAsAnOwnedRemovableTree() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(checkNotNull(InstrumentationRegistry.getArguments().getString("removalHardlinkRoot")))
        assertEquals(context.cacheDir.canonicalFile, root.parentFile!!.canonicalFile)
        assertTrue(root.name.matches(Regex("removal-hardlink-[0-9a-f-]{36}")))
        val id = "a".repeat(64)
        try {
            val first = File(root, "$id/one"); val second = File(root, "$id/two")
            assertEquals(2L, Os.lstat(first.path).st_nlink)
            assertEquals(Os.lstat(first.path).st_ino, Os.lstat(second.path).st_ino)
            val failure = checkNotNull(PluginReleaseCatalog.open(root)).use {
                runCatching { PluginReleaseRemoval.prepare(it, id, null).close() }.exceptionOrNull()
            }
            assertTrue("actual hard links were accepted for removal: $failure", failure is PluginReleaseRemoval.Rejected && failure.reason == PluginReleaseRemoval.Reason.INVALID)
            assertEquals("content", first.readText()); assertEquals("content", second.readText())
            assertFalse(File(root, PluginReleaseCatalog.REMOVAL).exists())
        } finally { PluginInstallTestFiles.clean(root) }
    }
}
