package com.hanxi.ghosterm.terminal

import android.content.Context
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.fonts.Font
import android.graphics.fonts.FontFamily
import android.system.Os
import android.system.OsConstants
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest

data class ImportedTerminalFont(val id:String,val label:String,val problem:String?=null)

/** App-private, content-addressed font copies. The source URI/name never becomes a filesystem path. */
class TerminalFontStore(context:Context) {
    private val root=File(context.filesDir,"fonts")
    private fun root(create:Boolean=false):File {
        if(!root.exists() && create) check(root.mkdir()) { "无法创建字体目录" }
        if(root.exists()) {
            check(Os.lstat(root.path).st_mode and OsConstants.S_IFMT==OsConstants.S_IFDIR) { "字体目录不是普通目录" }
            check(root.canonicalFile==File(root.parentFile!!.canonicalFile,"fonts")) { "字体目录不可使用链接" }
            Os.chmod(root.path,0x1c0)
        }
        return root
    }
    private fun hash(bytes:ByteArray)=MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
    fun file(id:String):File {
        require(id.matches(Regex("custom:[0-9a-f]{64}"))) { "字体编号无效" }
        return File(root(),id.removePrefix("custom:")+".ttf")
    }
    fun verifiedFile(id:String):File=synchronized(lock) {
        val path=file(id)
        check(path.isFile && Os.lstat(path.path).st_mode and OsConstants.S_IFMT==OsConstants.S_IFREG) { "导入字体已丢失或不是普通文件" }
        check(path.length() in 1..MAX_FONT_BYTES.toLong()) { "字体文件大小无效" }
        check(hash(path.readBytes())==id.removePrefix("custom:")) { "字体文件发生变化，请重新导入" }
        path
    }
    fun list():List<ImportedTerminalFont> = synchronized(lock) {
        if(!root().exists()) return@synchronized emptyList()
        val files=checkNotNull(root.listFiles()) { "无法读取字体目录" }
        check(files.size<=MAX_FONTS*4) { "字体目录条目过多" }
        files.filter { it.name.matches(Regex("[0-9a-f]{64}\\.ttf")) }.map { path ->
            val id="custom:"+path.name.removeSuffix(".ttf")
            var problem:String?=null
            val label=try {
                verifiedFile(id)
                val metadata=File(root,path.name.removeSuffix(".ttf")+".json")
                check(Os.lstat(metadata.path).st_mode and OsConstants.S_IFMT==OsConstants.S_IFREG && metadata.length()<=1024)
                val value=JSONObject(metadata.readText());check(value.getString("id")==id)
                value.getString("name").also { check(it.isNotBlank() && it.length<=80 && it.none(Char::isISOControl)) }
            } catch(e:Exception) { problem=e.message ?: "字体无法读取";"导入字体 · "+id.takeLast(8) }
            ImportedTerminalFont(id,label,problem)
        }.sortedBy { it.label.lowercase() }
    }
    fun import(name:String,bytes:ByteArray,checkActive:()->Unit={}):ImportedTerminalFont=synchronized(lock) {
        checkActive()
        MonospaceFont.validate(bytes)
        val directory=root(true)
        val id="custom:"+hash(bytes);val target=file(id)
        val label=name.substringAfterLast('/').substringAfterLast('\\')
            .filterNot { it.isISOControl() || it.code in 0x202a..0x202e || it.code in 0x2066..0x2069 }.take(80).ifBlank { "导入的字体" }
        if(target.exists()) {
            verifiedFile(id)
            return@synchronized list().single { it.id==id }.also { check(it.problem==null) { "已有字体记录损坏，请先移除后重新导入" } }
        }
        val existing=checkNotNull(directory.listFiles())
        val metadataBytes=JSONObject().put("id",id).put("name",label).toString().toByteArray()
        check(existing.all { Os.lstat(it.path).st_mode and OsConstants.S_IFMT==OsConstants.S_IFREG }) { "字体目录包含非普通文件" }
        check(existing.count { it.name.endsWith(".ttf") }<MAX_FONTS) { "最多保存 16 个导入字体，请先移除不用的字体" }
        check(existing.sumOf { it.length() }+bytes.size+metadataBytes.size<=MAX_TOTAL_BYTES) { "字体存储空间已达 32 MiB，请先移除不用的字体" }
        val meta=File(directory,target.nameWithoutExtension+".json")
        check(!meta.exists()) { "字体名称记录已存在，请先核对已有字体" }
        val stage=File.createTempFile(".import-",".stage",directory)
        var metaStage:File?=null
        try {
            val nameStage=File.createTempFile(".name-",".stage",directory).also { metaStage=it }
            FileOutputStream(stage).use { it.write(bytes);it.fd.sync() };Os.chmod(stage.path,0x180)
            val face=Typeface.CustomFallbackBuilder(FontFamily.Builder(Font.Builder(stage).build()).build()).setSystemFallback("monospace").build()
            val paint=Paint().apply { typeface=face;textSize=32f }
            val width=paint.measureText("M")
            check(width>0 && (32..126).all { kotlin.math.abs(paint.measureText(it.toChar().toString())-width)<.05f }) { "该字体不能按等宽网格显示" }
            checkActive()
            Files.move(stage.toPath(),target.toPath(),StandardCopyOption.ATOMIC_MOVE)
            FileOutputStream(nameStage).use { output -> output.write(metadataBytes);output.fd.sync() }
            Os.chmod(nameStage.path,0x180)
            Files.move(nameStage.toPath(),meta.toPath(),StandardCopyOption.ATOMIC_MOVE)
            val fd=Os.open(directory.path,OsConstants.O_RDONLY or OsConstants.O_CLOEXEC,0)
            try { check(Os.fstat(fd).st_mode and OsConstants.S_IFMT==OsConstants.S_IFDIR);Os.fsync(fd) } finally { Os.close(fd) }
            ImportedTerminalFont(id,label)
        } finally { stage.delete();metaStage?.delete() }
    }
    fun remove(id:String)=synchronized(lock) {
        val path=file(id)
        check(path.exists() && Os.lstat(path.path).st_mode and OsConstants.S_IFMT==OsConstants.S_IFREG) { "字体已变化，请重新载入" }
        check(path.delete()) { "无法移除字体" }
        val metadata=File(path.parentFile,path.nameWithoutExtension+".json")
        if(metadata.exists()) check(metadata.delete()) { "字体已移除，但名称信息清理失败" }
    }
    companion object {
        const val MAX_FONT_BYTES=4*1024*1024
        const val MAX_FONTS=16
        const val MAX_TOTAL_BYTES=32L*1024*1024
        private val lock=Any()
    }
}
