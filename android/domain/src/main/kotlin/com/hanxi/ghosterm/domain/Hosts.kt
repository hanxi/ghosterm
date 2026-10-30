package com.hanxi.ghosterm.domain

/** A derived view of ssh_config. Never persisted as a second host database. */
data class FieldSource(val explicitValue: String?, val path: String, val header: String, val line: Int,
                       val included: Boolean, val effectiveIsExplicit: Boolean) {
    val description: String get() {
        val origin = when {
            line == 0 -> "默认值（未显式设置）"
            effectiveIsExplicit -> "此主机显式设置 · 第 $line 行"
            included -> "继承自 $path · 第 $line 行"
            header.isBlank() -> "继承自全局配置 · 第 $line 行"
            else -> "继承自 ${header.trim()} · 第 $line 行"
        }
        return if (explicitValue != null && !effectiveIsExplicit) "$origin；此块设置的 $explicitValue 未生效" else origin
    }
}
data class HostSource(val header: String, val path: String, val line: Int, val patternCount: Int,
                      val matchingBlocks: Int, val included: Boolean,
                      val hostname: FieldSource, val user: FieldSource, val port: FieldSource) {
    val editable: Boolean get() = !included && matchingBlocks == 1
    val editRestriction: String? get() = when {
        included -> "此主机来自包含文件 $path，第 $line 行；当前表单不能修改包含文件。"
        matchingBlocks != 1 -> "此别名出现在 $matchingBlocks 个 Host 块中，无法确定修改目标；请在 SSH 配置页编辑。"
        else -> null
    }
}

data class Host(val alias: String, val hostname: String, val user: String, val port: Int, val source: HostSource? = null) {
    val address: String get() = (if (user.isEmpty()) "" else "$user@") + hostname + ":$port"
}

enum class HostField(val keyword: String, val label: String) {
    HOSTNAME("HostName", "主机地址"), USER("User", "远端用户名"), PORT("Port", "端口");

    fun source(host: Host?): FieldSource? = when (this) {
        HOSTNAME -> host?.source?.hostname; USER -> host?.source?.user; PORT -> host?.source?.port
    }
    fun effective(host: Host): String = when (this) {
        HOSTNAME -> host.hostname; USER -> host.user; PORT -> host.port.toString()
    }
}

/** Explicit values and inheritance are independent of the currently effective value. */
data class HostEdit(val originalAlias: String?, val alias: String, val hostname: String, val user: String,
                    val port: String, val inherited: Set<HostField> = emptySet(),
                    // null leaves all identity lines untouched; empty removes only this block's lines.
                    val identityFiles: List<String>? = null) {
    companion object {
        fun from(host: Host?): HostEdit {
            if (host == null) return HostEdit(null, "", "", "", "22")
            fun initial(field: HostField) = field.source(host)?.explicitValue ?: field.effective(host)
            return HostEdit(host.alias, host.alias, initial(HostField.HOSTNAME), initial(HostField.USER), initial(HostField.PORT),
                HostField.entries.filter { it.source(host)?.explicitValue == null }.toSet())
        }
    }
    fun value(field: HostField): String = when (field) {
        HostField.HOSTNAME -> hostname; HostField.USER -> user; HostField.PORT -> port
    }
    fun withValue(field: HostField, value: String): HostEdit = when (field) {
        HostField.HOSTNAME -> copy(hostname = value); HostField.USER -> copy(user = value); HostField.PORT -> copy(port = value)
    }
    fun withInheritance(field: HostField, enabled: Boolean) =
        copy(inherited = if (enabled) inherited + field else inherited - field)
    fun normalized() = copy(alias = alias.trim(), hostname = hostname.trim(), user = user.trim(), port = port.trim())
    fun isDirty(previous: Host?): Boolean {
        if (identityFiles != null) return true
        if (previous == null) return this != from(null)
        return alias != previous.alias || HostField.entries.any { field ->
            val explicit = field.source(previous)?.explicitValue
            if (field in inherited) explicit != null else value(field) != explicit
        }
    }
    fun validate() {
        identityFiles?.let { values ->
            require(values.size <= 256) { "最多设置 256 条身份" }
            require(values.all { it.isNotEmpty() && it.none { c -> c == '\u0000' || c == '\r' || c == '\n' } }) { "身份引用不能为空或包含换行" }
        }
        require(alias.matches(Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,127}"))) { "别名只能包含字母、数字、点、短横线与下划线" }
        fun validToken(value: String) = value.isNotBlank() && value.none { it.isWhitespace() || it in "\u0000\r\n#\"'\\" }
        if (HostField.HOSTNAME !in inherited) require(validToken(hostname)) { "请填写有效的主机地址，或选择使用继承值" }
        if (HostField.USER !in inherited) require(validToken(user)) { "请填写远端用户名，或选择使用继承值" }
        if (HostField.PORT !in inherited) require(port.toIntOrNull() in 1..65535) { "端口必须为 1–65535，或选择使用继承值" }
    }
    fun changes(previous: Host?): Map<String, String?> {
        validate()
        return buildMap {
            for (field in HostField.entries) {
                val explicit = field.source(previous)?.explicitValue
                if (field in inherited) {
                    if (explicit != null) put(field.keyword, null)
                } else if (value(field) != explicit) put(field.keyword, value(field))
            }
        }
    }
}

class ConfigConflict : IllegalStateException("SSH 配置已改变，请重新载入后再编辑")
class ConfigAccess(message: String, cause: Throwable? = null) : IllegalStateException(message, cause)
