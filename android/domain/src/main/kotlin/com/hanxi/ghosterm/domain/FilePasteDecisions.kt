package com.hanxi.ghosterm.domain

import java.util.Collections
import java.util.UUID

enum class FilePasteChoice { KEEP_BOTH, SKIP }
enum class FilePasteDecisionKind { CONFLICT, UNSUPPORTED }
data class FilePasteDecisionRef(val sessionId: String, val requestId: String, val revision: Long)
data class FilePasteQuestion(val source: FileScanEntry, val kind: FilePasteDecisionKind, val existing: BrowserEntry?)
data class FileCopyRoot(val source: FileScanEntry, val destinationName: String, val onConflict: FileConflictPolicy)
data class FileCopyEntry(val source: FileScanEntry, val destinationRelativePath: String)
data class FileCopySkip(val source: FileScanEntry, val reason: FilePasteDecisionKind)

private fun <T> frozen(items: Collection<T>): List<T> = Collections.unmodifiableList(ArrayList(items))
class FilePasteDecisionState internal constructor(val reference: FilePasteDecisionRef,
    choices: Map<String,FilePasteChoice>, pending: List<FilePasteQuestion>,
    val remainingPolicy: FileConflictPolicy, val cancelled: Boolean, val confirmed: Boolean) {
    val choices: Map<String,FilePasteChoice> = Collections.unmodifiableMap(LinkedHashMap(choices))
    val pending: List<FilePasteQuestion> = frozen(pending)
}

/** Frozen decisions and logical destination names. No URI/path reservation or content snapshot.
 * Writers must revalidate source/target and use exclusive creation/non-overwrite publication. */
class FileCopyPlan internal constructor(val id: String, val check: FilePasteCheck,
    roots: List<FileCopyRoot>, entries: List<FileCopyEntry>, skipped: List<FileCopySkip>,
    val remainingPolicy: FileConflictPolicy, val knownBytes: ULong, val unknownSizeFiles: Int) {
    val roots: List<FileCopyRoot> = frozen(roots)
    val entries: List<FileCopyEntry> = frozen(entries)
    val skipped: List<FileCopySkip> = frozen(skipped)
    val request get() = check.scan.request
    val hasWork get() = entries.isNotEmpty()
    val totalBytes: ULong? get() = if(unknownSizeFiles==0) knownBytes else null
}

/** One confirmation owner. Ref+revision rejects late UI events and stale confirmations.
 * All methods are serialized; no global policy, IO, credentials or live connection is retained. */
class FilePasteDecisions private constructor(private val check: FilePasteCheck) {
    private val request=check.scan.request
    private val sessionId=UUID.randomUUID().toString()
    private var revision=0L
    private var cancelled=false
    private var confirmed=false
    private var remainingPolicy=request.conflictPolicy
    private val choices=linkedMapOf<String,FilePasteChoice>()
    private val entries=check.scan.entries
    private val nodes=entries.associateBy { it.relativePath }
    private val roots=request.items.map { nodes.getValue(it.name) }
    private val conflicts=check.conflicts.associateBy { it.source.name }
    private val questions=entries.mapNotNull { entry ->
        when {
            entry.entry.kind==FileKind.LINK || entry.entry.kind==FileKind.OTHER -> FilePasteQuestion(entry,FilePasteDecisionKind.UNSUPPORTED,null)
            entry.relativePath in conflicts -> FilePasteQuestion(entry,FilePasteDecisionKind.CONFLICT,conflicts.getValue(entry.relativePath).destination)
            else -> null
        }
    }
    private val questionsByPath=questions.associateBy { it.source.relativePath }
    init {
        for(question in questions.filter { it.kind==FilePasteDecisionKind.CONFLICT }) when(request.conflictPolicy) {
            FileConflictPolicy.ASK -> Unit
            FileConflictPolicy.KEEP_BOTH -> choices[question.source.relativePath]=FilePasteChoice.KEEP_BOTH
            FileConflictPolicy.SKIP -> choices[question.source.relativePath]=FilePasteChoice.SKIP
        }
    }
    companion object {
        fun begin(check: FilePasteCheck): FilePasteDecisions {
            require(check.checked && check.scan.complete && check.create!=FileCreateCapability.DENIED) { "预检尚未完成，不能确认复制" }
            validate(check)
            return FilePasteDecisions(check)
        }
        private fun validate(check: FilePasteCheck) {
            val scan=check.scan;val request=scan.request
            require(scan.entries.size in 1..16384) { "扫描条目数量无效" }
            val nodes=scan.entries.associateBy { it.relativePath }
            require(nodes.size==scan.entries.size) { "扫描路径重复" }
            val seen=HashSet<String>()
            val children=HashMap<String,Int>()
            for(node in scan.entries) {
                FileCopyNames.validate(node.entry.name)
                val segments=node.relativePath.split('/')
                require(segments.size in 1..64 && segments.all { it.isNotEmpty() && it!="." && it!=".." } && segments.last()==node.entry.name) { "扫描相对路径无效" }
                require(node.parent.endpoint==request.source.endpoint) { "扫描来源端点已改变" }
                if(segments.size==1) require(node.parent==request.source) { "扫描根不属于来源目录" }
                else {
                    val parent=node.relativePath.substringBeforeLast('/')
                    val owner=nodes[parent]
                    require(parent in seen && owner?.entry?.kind==FileKind.DIRECTORY && node.parent.directoryId==owner.entry.id) { "扫描父目录缺失或归属无效" }
                    children[parent]=(children[parent]?:0)+1
                }
                seen.add(node.relativePath)
            }
            val rootNodes=scan.entries.filter { '/' !in it.relativePath }
            require(rootNodes.map { it.entry.id }==request.items.map { it.id } && rootNodes.map { it.entry.name }==request.items.map { it.name } &&
                rootNodes.map { it.entry.kind }==request.items.map { it.kind }) { "扫描根与复制清单不一致" }
            for(node in scan.entries) if(node.entry.kind==FileKind.DIRECTORY)
                require(node.childCount!=null && node.childCount==(children[node.relativePath]?:0)) { "目录扫描不完整" }
            require(check.targetEntries.map { it.name }.toSet().size==check.targetEntries.size) { "目标名称不唯一" }
            val existing=check.targetEntries.associateBy { it.name }
            val expected=request.items.mapNotNull { item -> existing[item.name]?.let { FilePasteConflict(item,it) } }
            require(check.conflicts==expected) { "冲突不属于当前预检" }
        }
    }
    private fun ref()=FilePasteDecisionRef(sessionId,request.id,revision)
    private fun active(expected: FilePasteDecisionRef) {
        require(expected==ref()) { "确认已过期，请使用当前复制请求" }
        check(!cancelled && !confirmed) { "本次复制确认已结束" }
    }
    private fun covered(path: String)=path.contains('/') && choices[path.substringBefore('/')]==FilePasteChoice.SKIP
    private fun pending()=questions.filter { it.source.relativePath !in choices && !covered(it.source.relativePath) }
    @Synchronized fun state()=FilePasteDecisionState(ref(),choices,pending(),remainingPolicy,cancelled,confirmed)
    @Synchronized fun choose(expected: FilePasteDecisionRef, path: String, choice: FilePasteChoice,
        applyToRemaining: Boolean=false): FilePasteDecisionState {
        active(expected)
        val question=questionsByPath[path]
        require(question!=null && !covered(path)) { "此条目没有可处理的冲突或已被跳过" }
        if(question.kind==FilePasteDecisionKind.UNSUPPORTED)
            require(choice==FilePasteChoice.SKIP && !applyToRemaining) { "不支持的条目只能显式跳过" }
        choices[path]=choice
        if(applyToRemaining) {
            remainingPolicy=if(choice==FilePasteChoice.KEEP_BOTH) FileConflictPolicy.KEEP_BOTH else FileConflictPolicy.SKIP
            for(other in questions.filter { it.kind==FilePasteDecisionKind.CONFLICT && it.source.relativePath !in choices })
                choices[other.source.relativePath]=choice
        }
        revision++
        return state()
    }
    @Synchronized fun cancel(expected: FilePasteDecisionRef): FilePasteDecisionState {
        active(expected);cancelled=true;revision++;return state()
    }
    @Synchronized fun confirm(expected: FilePasteDecisionRef, maxRenameAttempts: Int=10000,
        maxMetadataBytes: Int=16*1024*1024): FileCopyPlan {
        active(expected)
        check(pending().isEmpty()) { "仍有冲突或不支持条目等待决定" }
        require(maxRenameAttempts in 1..10000 && maxMetadataBytes in 1..16*1024*1024)
        val occupied=check.targetEntries.mapTo(HashSet()) { it.name }
        // Reserve incoming unchanged roots too: renaming a.txt must not steal a (2).txt.
        roots.filter { choices[it.relativePath]!=FilePasteChoice.SKIP && it.relativePath !in conflicts }
            .forEach { occupied.add(it.entry.name) }
        val plannedRoots=roots.filter { choices[it.relativePath]!=FilePasteChoice.SKIP }.map { root ->
            val keepBoth=choices[root.relativePath]==FilePasteChoice.KEEP_BOTH
            val name=if(keepBoth) FileCopyNames.next(root.entry.name,root.entry.kind,occupied,maxRenameAttempts) else root.entry.name
            occupied.add(name)
            FileCopyRoot(root,name,if(keepBoth) FileConflictPolicy.KEEP_BOTH else remainingPolicy)
        }
        val byRoot=plannedRoots.associateBy { it.source.relativePath }
        val planned=ArrayList<FileCopyEntry>();val skipped=ArrayList<FileCopySkip>()
        var known=0uL;var unknown=0;var metadata=0L
        for(node in entries) {
            val root=node.relativePath.substringBefore('/')
            val skip=choices[root]==FilePasteChoice.SKIP || choices[node.relativePath]==FilePasteChoice.SKIP
            if(skip) {
                val decisionPath=if(choices[root]==FilePasteChoice.SKIP) root else node.relativePath
                val reason=questionsByPath.getValue(decisionPath).kind
                skipped.add(FileCopySkip(node,reason));continue
            }
            check(node.entry.kind==FileKind.FILE || node.entry.kind==FileKind.DIRECTORY) { "不支持条目尚未跳过" }
            val path=byRoot.getValue(root).destinationName+node.relativePath.removePrefix(root)
            metadata+=path.toByteArray(Charsets.UTF_8).size
            check(metadata<=maxMetadataBytes) { "目标路径计划超过元数据预算" }
            planned.add(FileCopyEntry(node,path))
            if(node.entry.kind==FileKind.FILE) {
                val size=node.entry.size
                if(size==null) unknown++ else { check(ULong.MAX_VALUE-known>=size) { "计划文件总量溢出" };known+=size }
            }
        }
        val plan=FileCopyPlan(UUID.randomUUID().toString(),check,plannedRoots,planned,skipped,remainingPolicy,known,unknown)
        confirmed=true;revision++
        return plan
    }
}

/** Candidate names only. Final creation must be exclusive and retry conflicts within a bound. */
object FileCopyNames {
    internal fun validate(name: String) {
        require(name.isNotEmpty() && name!="." && name!=".." && '/' !in name && '\u0000' !in name) { "无效的复制名称" }
        var i=0
        while(i<name.length) {
            val c=name[i++]
            require(!Character.isLowSurrogate(c)) { "复制名称包含无效字符" }
            if(Character.isHighSurrogate(c)) require(i<name.length && Character.isLowSurrogate(name[i++])) { "复制名称包含无效字符" }
        }
    }
    fun next(original: String, kind: FileKind, occupied: Set<String>, maxAttempts: Int=10000): String {
        validate(original)
        require(kind==FileKind.FILE || kind==FileKind.DIRECTORY)
        require(maxAttempts in 1..10000)
        val dot=original.lastIndexOf('.')
        val extension=if(kind==FileKind.FILE && dot>0 && original.substring(dot).toByteArray(Charsets.UTF_8).size<=64) original.substring(dot) else ""
        val stem=original.removeSuffix(extension)
        for(index in 2..maxAttempts+1) {
            val suffix=" ($index)"
            val budget=255-suffix.toByteArray(Charsets.UTF_8).size-extension.toByteArray(Charsets.UTF_8).size
            val prefix=StringBuilder();var bytes=0;var offset=0
            while(offset<stem.length) {
                val cp=stem.codePointAt(offset)
                val size=when { cp<=0x7f -> 1;cp<=0x7ff -> 2;cp<=0xffff -> 3;else -> 4 }
                if(bytes+size>budget) break
                prefix.appendCodePoint(cp);bytes+=size;offset+=Character.charCount(cp)
            }
            val candidate=prefix.toString()+suffix+extension
            if(candidate !in occupied) return candidate
        }
        error("无法在编号预算内找到新名称，请刷新或跳过")
    }
}
