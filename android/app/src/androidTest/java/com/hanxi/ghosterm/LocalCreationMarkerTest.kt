package com.hanxi.ghosterm

import android.system.Os
import android.system.OsConstants
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import org.junit.*
import org.junit.Assert.*
import java.io.File
import java.util.UUID

/** Real app UID/SELinux/filesystem capability, with no root or policy changes. */
class LocalCreationMarkerTest {
    @Test fun fdCreationMarkerSurvivesReadOnlyOpenAndRename() {
        val context=InstrumentationRegistry.getInstrumentation().targetContext
        val root=File(context.cacheDir,"creation-marker-${UUID.randomUUID()}").apply { mkdirs() }
        try {
            val token=UUID.randomUUID().toString().toByteArray(Charsets.US_ASCII)
            LocalPublicationFile.directory(root,"").use { parent ->
                LocalPublicationFile.candidate(parent.fd,"candidate").use { writer ->
                    val set=runCatching { Os.setxattr("/proc/self/fd/${writer.fd}","user.ghosterm.copy",token,1 /* XATTR_CREATE, Linux UAPI; not exposed by OsConstants */) }
                    assertTrue("Private app filesystem must support creation markers: ${set.exceptionOrNull()}",set.isSuccess)
                    assertArrayEquals(token,Os.getxattr("/proc/self/fd/${writer.fd}","user.ghosterm.copy"))
                }
                assertTrue(File(root,"candidate").renameTo(File(root,"published")))
                LocalPublicationFile.retainedCandidate(parent.fd,"published").use { reader ->
                    assertArrayEquals(token,Os.getxattr("/proc/self/fd/${reader.fd}","user.ghosterm.copy"))
                    var rejected=false
                    try { Os.setxattr("/proc/self/fd/${reader.fd}","user.ghosterm.copy",token,1 /* XATTR_CREATE, Linux UAPI; not exposed by OsConstants */) }
                    catch(error:android.system.ErrnoException) { rejected=error.errno==OsConstants.EEXIST }
                    assertTrue("Existing provenance cannot be silently replaced",rejected)
                }
            }
        } finally { root.deleteRecursively() }
    }
}
