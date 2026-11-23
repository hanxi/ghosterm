package com.hanxi.ghosterm

import com.hanxi.ghosterm.bridge.ExtensionPackage
import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest

/** Presentation only, after the complete ZIP passed Core capture/check. */
internal object PluginReleaseDeclarations {
    const val manifestName = "manifest.json"

    class Rejected : IllegalArgumentException("包声明与 Core 固定清单摘要不一致或格式无效。")
    data class Permission(val side: String, val capability: String, val purpose: String, val scope: String, val programs: List<String>)
    data class Data(val manifestSHA256: String, val sdkRange: String, val selectedSDK: String, val protocolRange: List<Int>, val protocols: List<Int>,
        val permissions: List<Permission>, val executorJSON: String?)
    fun decode(bytes: ByteArray, metadata: ExtensionPackage.Metadata): Data {
        if (bytes.size > 65536 || hash(bytes) != metadata.manifestDigest) throw Rejected()
        val root = PluginReleaseSignature.declarationObject(bytes)
        fun text(value: JSONObject, key: String) = value.opt(key) as? String ?: throw Rejected()
        if (root.opt("schemaVersion") != 2) throw Rejected()
        val permissions = luaPermissions(root)
        val executor = root.opt("executor")?.let { (it as? JSONObject ?: throw Rejected()).toString(2) }
        val protocolValues = root.opt("bridgeProtocolRange") as? JSONArray ?: throw Rejected()
        val declared = (0 until protocolValues.length()).map {
            val value = protocolValues.opt(it) as? Number ?: throw Rejected()
            val version = value.toInt()
            if (version !in 1..65535 || value.toDouble() != version.toDouble()) throw Rejected()
            version
        }
        return Data(metadata.manifestDigest, text(root, "sdkApiRange"), metadata.sdkVersion, declared, metadata.protocols.toList(), permissions, executor)
    }
    private fun luaPermissions(root: JSONObject): List<Permission> {
        if (!root.has("client") && !root.has("executor")) throw Rejected()
        return listOf("client", "executor").flatMap { side ->
            val role = root.opt(side)?.let { it as? JSONObject ?: throw Rejected() } ?: return@flatMap emptyList()
            val requirements = role.optJSONArray("requires") ?: throw Rejected()
            val names = role.opt("programs")?.let { it as? JSONArray ?: throw Rejected() }
            val programs = if (names == null) emptyList() else (0 until names.length()).map {
                names.opt(it) as? String ?: throw Rejected()
            }
            (0 until requirements.length()).map {
                val capability = requirements.opt(it) as? String ?: throw Rejected()
                Permission(side, capability, "", "", if (capability == "process.spawn") programs else emptyList())
            }
        }
    }

    private fun hash(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }
}
