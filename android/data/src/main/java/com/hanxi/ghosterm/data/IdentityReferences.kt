package com.hanxi.ghosterm.data

import com.hanxi.ghosterm.bridge.ConfigBridge
import com.hanxi.ghosterm.bridge.ConfigIdentityInfo
import java.util.UUID

/** An app-local reference in ssh_config, NOT a plaintext key path or an exported SSH key. */
object IdentityReferences {
    const val PREFIX = "ghosterm-identity:"
    fun encode(id: String): String {
        require(canonical(id)) { "身份编号无效" }
        return PREFIX + id
    }
    fun decode(value: String): String? = value.takeIf { it.startsWith(PREFIX) }
        ?.removePrefix(PREFIX)?.takeIf(::canonical)
    private fun canonical(value: String): Boolean = try { UUID.fromString(value).toString() == value } catch (_: IllegalArgumentException) { false }

    fun candidates(values: List<String>, available: List<StoredIdentity>): IdentityCandidates {
        val byId = available.associateBy { it.id }
        val identities = mutableListOf<StoredIdentity>()
        val issues = mutableListOf<String>()
        for (value in values) {
            val id = decode(value)
            when {
                id != null -> byId[id]?.let { if (it !in identities) identities += it }
                    ?: run { issues += "配置身份已移除或不可用：$value" }
                value.startsWith(PREFIX) -> issues += "配置身份引用无效：$value"
                value == "none" -> issues += "配置包含 IdentityFile none；使用密钥需明确选择本次身份。"
                else -> issues += "尚未关联的身份路径：$value。可在主机编辑中关联，或仅为本次连接选择身份。"
            }
        }
        return IdentityCandidates(identities, issues)
    }

    /** Core is the only evaluator. Include/inherited and inactive own-block references retain provenance. */
    fun usages(snapshot: ConfigSnapshot, id: String, checkpoint: () -> Unit = {}): List<IdentityUsage> = buildList {
        val reference = encode(id)
        for (host in snapshot.hosts) {
            checkpoint()
            val sources = ConfigBridge.identities(snapshot.bytes, host.alias).filter { it.value == reference }
            if (sources.isNotEmpty()) add(IdentityUsage(host.alias, sources))
        }
    }
}

data class IdentityCandidates(val identities: List<StoredIdentity>, val issues: List<String>)
data class IdentityUsage(val alias: String, val sources: List<ConfigIdentityInfo>)

fun ConfigIdentityInfo.originDescription(): String =
    (if (explicitValue) "此 Host 块" else "继承") + " · " +
        sourcePath.ifEmpty { "应用内 SSH 配置" } + ":$sourceLine" +
        (if (included) " · Include" else "") + (if (!effective) " · 当前别名不生效" else "")
