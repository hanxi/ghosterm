package com.hanxi.ghosterm

import android.content.Context
import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption

class PluginGitHubInstallTransaction(private val context: Context) {
    fun stage(plan: PluginInstallPlan, assets: List<Triple<ByteArray, ByteArray, ByteArray>>): File {
        require(plan.isValid && assets.size == plan.items.size)
        val root = File(context.filesDir, ".plugin-install-${System.nanoTime()}")
        require(root.mkdirs())
        try {
            plan.items.zip(assets).forEach { (item, bytes) ->
                val version = File(root, item.id).apply { require(mkdirs()) }
                bytes.first.writeAtomic(File(version, "payload.zip"))
                bytes.second.writeAtomic(File(version, "release.json"))
                bytes.third.writeAtomic(File(version, "release.sig"))
            }
            return root
        } catch (failure: Throwable) { root.deleteRecursively(); throw failure }
    }
    fun commit(staged: File, activeRoot: File) {
        require(staged.isDirectory)
        val backup = File(activeRoot.parentFile, activeRoot.name + ".previous-${System.nanoTime()}")
        try {
            if (activeRoot.exists()) Files.move(activeRoot.toPath(), backup.toPath(), StandardCopyOption.ATOMIC_MOVE)
            Files.move(staged.toPath(), activeRoot.toPath(), StandardCopyOption.ATOMIC_MOVE)
            backup.deleteRecursively()
        } catch (failure: Throwable) {
            if (!activeRoot.exists() && backup.exists()) Files.move(backup.toPath(), activeRoot.toPath(), StandardCopyOption.ATOMIC_MOVE)
            staged.deleteRecursively(); throw failure
        }
    }
    fun discard(staged: File) { staged.deleteRecursively() }
}
private fun ByteArray.writeAtomic(target: File) {
    val temporary = File(target.parentFile, ".${target.name}.tmp-${System.nanoTime()}")
    temporary.writeBytes(this); if (!temporary.renameTo(target)) { temporary.delete(); error("原子写入失败") }
}
