package com.hanxi.ghosterm.data

import com.hanxi.ghosterm.domain.*
import org.json.JSONArray
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.util.Collections
import java.util.UUID

/** Versioned, bounded observations. No decoder produces an executable request. */
internal object FileJournalCodec {
    const val HEADER_LIMIT = 1024 * 1024
    const val ITEM_LIMIT = 64 * 1024
    private fun obj(vararg values: Pair<String, Any?>) = JSONObject().apply {
        values.forEach { (key,value) -> put(key,value ?: JSONObject.NULL) }
    }
    private fun location(v: FileLocation): JSONObject = obj("id" to v.directoryId,
        "endpoint" to obj("id" to v.endpoint.id,"label" to v.endpoint.label,"root" to v.endpoint.rootId,
            "tree" to v.endpoint.treeUri,"remote" to v.endpoint.remote?.let {
                obj("alias" to it.alias,"host" to it.host,"user" to it.user,"port" to it.port.toString())
            }))
    private fun entry(v: BrowserEntry) = obj("id" to v.id,"name" to v.name,"kind" to v.kind.name,
        "size" to v.size?.toString(),"modified" to v.modifiedMillis?.toString())
    private fun source(v: FileScanEntry) = obj("parent" to location(v.parent),"entry" to entry(v.entry),
        "path" to v.relativePath,"children" to v.childCount?.toString())
    private fun bytes(o: JSONObject, limit: Int): ByteArray {
        // String.toByteArray replaces unpaired surrogates. A record must preserve
        // the original identity or fail, never silently save a different name.
        val encoded=try {
            Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(o.toString()))
        } catch(error: CharacterCodingException) {
            throw IllegalArgumentException("任务记录包含无效字符，未保存",error)
        }
        require(encoded.remaining()<=limit) { "任务记录条目过大" }
        return ByteArray(encoded.remaining()).also { encoded.get(it) }
    }
    fun header(v: FileJournalRecord): ByteArray {
        require(v.items.isEmpty())
        val i=v.intent
        return bytes(obj("version" to "1","id" to i.id,"clipboard" to i.clipboardId,
            "source" to location(i.source),"sourceLabel" to i.sourceLabel,
            "selected" to JSONArray(i.selected.map(::entry)),"destination" to location(i.destination),
            "destinationLabel" to i.destinationLabel,"phase" to v.phase.name,
            "updated" to v.updatedMillis.toString(),"outcome" to v.outcome?.name,
            "cancelled" to v.cancelled,"error" to v.error),HEADER_LIMIT).also { readHeader(it) }
    }
    fun item(v: FileJournalItem): ByteArray {
        val i=v.value
        return bytes(obj("version" to "1","source" to source(i.source),"state" to i.state.name,
            "parent" to i.targetParent?.let(::location),"name" to i.targetName,"created" to i.createdId,
            "uri" to i.documentUri,"candidate" to i.candidatePath,"bytes" to i.bytes.toString(),
            "error" to i.error,"core" to i.coreStatus?.toString(),"remote" to i.remoteStatus?.toString(),
            "checkpoint" to v.checkpoint.name),ITEM_LIMIT).also { readItem(it) }
    }
    private fun parse(bytes: ByteArray, limit: Int): JSONObject {
        require(bytes.size in 1..limit) { "任务记录大小无效" }
        val text=Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
        // Bound parser recursion BEFORE invoking Android's JSON parser.
        var depth=0;var quoted=false;var escaped=false
        for(c in text) {
            if(quoted) { if(escaped) escaped=false else if(c=='\\') escaped=true else if(c=='"') quoted=false }
            else when(c) {
                '"'->quoted=true
                '{','['->{ depth++;require(depth<=16) { "任务记录嵌套过深" } }
                '}',']'->{ depth--;require(depth>=0) }
                else->require(c in " \t\r\n:,-+.0123456789truefalsenullEe") { "任务记录不是有效 JSON" }
            }
        }
        require(!quoted && depth==0)
        return JSONObject(text).also { require(it.toString()==text) { "任务记录编码不一致" } }
    }
    private fun JSONObject.fields(vararg expected: String) {
        require(keys().asSequence().toSet()==expected.toSet()) { "任务记录字段不完整或版本不支持" }
    }
    private fun JSONObject.text(key: String): String = (get(key) as? String
        ?: error("任务记录字段类型无效")).also { s ->
        require(s.toByteArray(Charsets.UTF_8).size<=16384 && '\u0000' !in s) { "任务记录文本无效或过长" }
        var i=0
        while(i<s.length) {
            val c=s[i++]
            if(c.isHighSurrogate()) require(i<s.length && s[i++].isLowSurrogate())
            else require(!c.isLowSurrogate())
        }
    }
    private fun JSONObject.optional(key: String): String? = if(get(key)==JSONObject.NULL) null else text(key)
    private fun JSONObject.objectAt(key: String): JSONObject = get(key) as? JSONObject ?: error("任务记录对象无效")
    private fun JSONObject.optionalObject(key: String): JSONObject? = if(get(key)==JSONObject.NULL) null else objectAt(key)
    private fun uuid(value: String) = value.also { require(UUID.fromString(it).toString()==it) { "任务记录身份无效" } }
    private fun long(value: String) = value.toLong().also { require(it.toString()==value) }
    private fun ulong(value: String) = value.toULong().also { require(it.toString()==value) }
    private fun int(value: String) = long(value).also { require(it in Int.MIN_VALUE..Int.MAX_VALUE) }.toInt()
    private fun location(o: JSONObject): FileLocation {
        o.fields("id","endpoint")
        val e=o.objectAt("endpoint");e.fields("id","label","root","tree","remote")
        val remote=e.optionalObject("remote")?.let { r ->
            r.fields("alias","host","user","port")
            RemoteFileIdentity(r.text("alias"),r.text("host"),r.text("user"),int(r.text("port")).also { require(it in 1..65535) })
        }
        require(remote==null || e.optional("tree")==null)
        return FileLocation(FileEndpoint(e.text("id"),e.text("label"),e.text("root"),e.optional("tree"),remote),o.text("id"))
    }
    private fun entry(o: JSONObject): BrowserEntry {
        o.fields("id","name","kind","size","modified")
        return BrowserEntry(o.text("id"),o.text("name"),FileKind.valueOf(o.text("kind")),
            o.optional("size")?.let(::ulong),o.optional("modified")?.let(::long))
    }
    fun readHeader(bytes: ByteArray): FileJournalRecord {
        val o=parse(bytes,HEADER_LIMIT)
        o.fields("version","id","clipboard","source","sourceLabel","selected","destination","destinationLabel","phase","updated","outcome","cancelled","error")
        require(o.text("version")=="1") { "任务记录版本不支持" }
        val selected=o.get("selected") as? JSONArray ?: error("任务来源清单无效")
        require(selected.length() in 1..4096)
        val entries=(0 until selected.length()).map { entry(selected.getJSONObject(it)) }
        require(entries.distinctBy { it.id }.size==entries.size)
        return FileJournalRecord(FileJournalIntent(uuid(o.text("id")),uuid(o.text("clipboard")),
            location(o.objectAt("source")),o.text("sourceLabel"),Collections.unmodifiableList(entries),
            location(o.objectAt("destination")),o.text("destinationLabel")),FileJournalPhase.valueOf(o.text("phase")),
            long(o.text("updated")).also { require(it>=0) },o.optional("outcome")?.let(FileCopyState::valueOf),
            o.get("cancelled") as? Boolean ?: error("任务取消状态无效"),o.optional("error"))
    }
    fun readItem(bytes: ByteArray): FileJournalItem {
        val o=parse(bytes,ITEM_LIMIT)
        o.fields("version","source","state","parent","name","created","uri","candidate","bytes","error","core","remote","checkpoint")
        require(o.text("version")=="1")
        val s=o.objectAt("source");s.fields("parent","entry","path","children")
        val path=s.text("path");require(path.isNotEmpty() && path.split('/').all { it.isNotEmpty() && it!="." && it!=".." })
        return FileJournalItem(FileCopyItemResult(FileScanEntry(location(s.objectAt("parent")),entry(s.objectAt("entry")),path,
            s.optional("children")?.let(::int)?.also { require(it>=0) }),FileCopyItemState.valueOf(o.text("state")),
            o.optionalObject("parent")?.let(::location),o.optional("name"),o.optional("created"),o.optional("uri"),
            o.optional("candidate"),ulong(o.text("bytes")),o.optional("error"),o.optional("core")?.let(::int),
            o.optional("remote")?.let(::ulong)?.also { require(it<=UInt.MAX_VALUE) }?.toUInt()),FileCopyCheckpoint.valueOf(o.text("checkpoint")))
    }
}
