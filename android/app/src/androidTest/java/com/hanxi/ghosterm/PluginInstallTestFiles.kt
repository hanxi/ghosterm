package com.hanxi.ghosterm

import android.system.Os
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.nio.file.FileVisitResult
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.SimpleFileVisitor
import java.nio.file.attribute.BasicFileAttributes
import java.util.UUID

/** Only callers' explicitly owned fixture trees. Never follows package links. */
internal object PluginInstallTestFiles {
    fun root() = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "plugin-install-test-${UUID.randomUUID()}").apply { check(mkdir()) }
    fun clean(root: File) {
        if (!Files.exists(root.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS)) return
        Files.walkFileTree(root.toPath(), object : SimpleFileVisitor<Path>() {
            override fun preVisitDirectory(dir: Path, attrs: BasicFileAttributes): FileVisitResult {
                Os.chmod(dir.toString(), 448); return FileVisitResult.CONTINUE
            }
            override fun visitFile(file: Path, attrs: BasicFileAttributes): FileVisitResult {
                Files.delete(file); return FileVisitResult.CONTINUE
            }
            override fun postVisitDirectory(dir: Path, failure: java.io.IOException?): FileVisitResult {
                if (failure != null) throw failure
                Files.delete(dir); return FileVisitResult.CONTINUE
            }
        })
    }
}
