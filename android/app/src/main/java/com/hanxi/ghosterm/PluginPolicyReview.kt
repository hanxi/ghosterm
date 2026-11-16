package com.hanxi.ghosterm

import kotlinx.coroutines.CancellationException
import java.io.FileDescriptor

/** Read-only policy observation, then an explicit subset-only atomic disable.
 * Accepted rows do not grant source, Session or execution permission. */
internal class PluginPolicyReview private constructor(val value: Value, private val root: PluginReleaseRemoval.Stamp?) {
    enum class State { ACCEPTED, REJECTED, UNKNOWN }
    data class Entry(val pluginID: String, val releaseID: String, val state: State, val reason: String)
    data class Value(val id: String, val revision: String?, val channel: String, val policySHA256: String, val entries: List<Entry>)
    fun apply(owner: PluginReleaseCatalog, pluginIDs: List<String>, policy: ByteArray,
        checkpoint: (String) -> Unit = {}, sync: (FileDescriptor) -> Unit = { android.system.Os.fsync(it) }): PluginReleaseCatalog.Publication {
        require(pluginIDs.isNotEmpty() && pluginIDs.distinct().size == pluginIDs.size &&
            pluginIDs.all { id -> value.entries.any { it.pluginID == id && it.state != State.ACCEPTED } }) { "请选择本次复核中未被接受的明确子集" }
        check(root != null && root.sameObject(rootStamp(owner))) { "安装根目录已变化" }
        check(PluginReleaseCatalog.sha(policy) == value.policySHA256) { "本次策略快照已变化" }
        val selected = owner.selection()
        if (selected.revision != value.revision) throw PluginReleaseCatalog.Rejected(PluginReleaseCatalog.Reason.CONFLICT)
        for (id in pluginIDs) {
            val entry = value.entries.single { it.pluginID == id }
            val choice = selected.choices[id]
            check(choice != null && choice.enabled && choice.releaseID == entry.releaseID) { "所选活动选择已变化" }
        }
        return owner.disable(pluginIDs, value.revision, checkpoint, sync)
    }
    companion object {
        private fun rootStamp(owner: PluginReleaseCatalog) = owner.maintenanceRoot().use { PluginReleaseRemoval.stamp(it) }
        fun observe(owner: PluginReleaseCatalog?, id: String, policy: ByteArray,
            inspect: (Int) -> PluginReleaseSignature.Identity, checkpoint: () -> Unit = {}): PluginPolicyReview {
            val trusted = policy.copyOf()
            val channel = PluginReleaseSignature.policyChannel(trusted)
            val root = owner?.let(::rootStamp)
            val selected = owner?.selection() ?: PluginReleaseCatalog.Selection(null, emptyMap())
            val entries = selected.choices.toSortedMap().filterValues { it.enabled }.map { (pluginID, choice) ->
                checkpoint()
                val entry = try {
                    checkNotNull(owner).openVersion(choice.releaseID).use { version ->
                        version.pin()
                        val raw = version.read("release.json", 65536)
                        check(PluginReleaseCatalog.sha(raw) == choice.releaseID) { "发布描述已变化" }
                        val authenticated = PluginReleaseSignature.authenticate(raw, version.read("release.sig", 64), trusted)
                        version.payload().use { payload ->
                            val identity = inspect(payload.fd)
                            if (identity != authenticated.identity || identity.id != pluginID)
                                throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.IDENTITY)
                            owner.validateVersion(version, payload)
                        }
                    }
                    Entry(pluginID, choice.releaseID, State.ACCEPTED, "此策略快照接受签名和当前包内容；未授予来源或运行权限。")
                } catch (cancelled: CancellationException) { throw cancelled }
                catch (failure: Exception) {
                    val reason = (failure as? PluginReleaseSignature.Rejected)?.reason
                    if (reason in setOf(PluginReleaseSignature.Failure.REVOKED_KEY, PluginReleaseSignature.Failure.UNKNOWN_KEY,
                        PluginReleaseSignature.Failure.SCOPE, PluginReleaseSignature.Failure.SIGNATURE, PluginReleaseSignature.Failure.IDENTITY))
                        Entry(pluginID, choice.releaseID, State.REJECTED, checkNotNull(failure.message))
                    else Entry(pluginID, choice.releaseID, State.UNKNOWN, "无法完整读取或核验此选择，不能判断为已撤销或仍受信任。")
                }
                checkpoint(); entry
            }
            owner?.validate()
            if (owner != null && owner.selection().revision != selected.revision) throw PluginReleaseCatalog.Rejected(PluginReleaseCatalog.Reason.CONFLICT)
            return PluginPolicyReview(Value(id, selected.revision, channel, PluginReleaseCatalog.sha(trusted), entries), root)
        }
    }
}
