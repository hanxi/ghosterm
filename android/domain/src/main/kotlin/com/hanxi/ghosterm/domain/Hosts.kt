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

data class HostEdit(val originalAlias: String?, val alias: String, val hostname: String, val user: String, val port: String) {
    fun validate() {
        require(alias.matches(Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,127}"))) { "别名只能包含字母、数字、点、短横线与下划线" }
        require(hostname.isNotBlank() && hostname.none { it.isWhitespace() || it in "\u0000\r\n#\"'\\" }) { "请填写有效的主机地址" }
        require(user.isNotBlank() && user.none { it.isWhitespace() || it in "\u0000\r\n#\"'\\" }) { "请填写远端用户名" }
        require(port.toIntOrNull() in 1..65535) { "端口必须为 1–65535" }
        require(originalAlias == null || originalAlias == alias) { "别名修改请使用 SSH 配置编辑器" }
    }
    fun changes(previous: Host?): Map<String, String> {
        validate()
        return buildMap {
            if (previous == null || hostname != previous.hostname) put("HostName", hostname)
            if (previous == null || user != previous.user) put("User", user)
            if (previous == null || port.toInt() != previous.port) put("Port", port)
        }
    }
}

class ConfigConflict : IllegalStateException("SSH 配置已改变，请重新载入后再编辑")
class ConfigAccess(message: String, cause: Throwable? = null) : IllegalStateException(message, cause)
