package com.hanxi.ghosterm

import androidx.test.platform.app.InstrumentationRegistry
import java.io.File

/** A real child executable from the instrumentation APK, never an app hook. */
internal object NativePtyFixture {
    fun initialize(): String {
        val context = InstrumentationRegistry.getInstrumentation().context
        val executable = File(context.applicationInfo.nativeLibraryDir, "libghosterm_test_pty.so")
        check(executable.isFile && executable.canExecute()) { "Missing instrumentation PTY executable: $executable" }
        val command = "'" + executable.absolutePath.replace("'", "'\\''") + "'"
        // Restore the exact original settings rather than guessing what 'sane'
        // means on this OS. The snapshot exists only in this test shell.
        return "GT_TEST_TTY=\$($command save) || exit 91; " +
            "stty() { if [ \"\$1\" = sane ]; then $command restore \"\$GT_TEST_TTY\"; " +
            "else $command \"\$@\"; fi || exit 92; }; "
    }
}
