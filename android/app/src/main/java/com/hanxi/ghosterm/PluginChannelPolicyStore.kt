package com.hanxi.ghosterm

import java.io.File
import java.nio.ByteBuffer

/** Private, separately locked channel state. The host supplies the trust root on every open. */
internal class PluginChannelPolicyStore(
    private val directory: File,
    private val channel: String,
    root: ByteArray,
) {
    private val root = root.copyOf()
    private val rootDigest = PluginReleaseCatalog.sha(this.root).toByteArray(Charsets.US_ASCII)
    private val name = "channel-policy-v1-" + PluginReleaseCatalog.sha(channel.toByteArray(Charsets.UTF_8))
    private data class Record(val observed: ULong, val statement: ByteArray, val signature: ByteArray, val policy: ByteArray)

    fun refresh(statement: ByteArray, signature: ByteArray, policy: ByteArray, now: ULong): PluginReleaseSignature.VerifiedChannelPolicy {
        if (statement.size !in 1..65536 || policy.size > 131072) {
            throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.LIMIT)
        }
        if (signature.size != 64) throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.CHANNEL_STATEMENT)
        val candidate = Record(now, statement.copyOf(), signature.copyOf(), policy.copyOf())
        val verified = verify(candidate, now)
        val owner = PluginReleaseCatalog.open(directory) ?: reject()
        return owner.use {
            val raw = it.readChannelRecord(name)
            if (raw != null) {
                val old = decode(raw)
                if (now < old.observed) reject()
                val previous = verify(old, old.observed)
                if (verified.generation < previous.generation ||
                    (verified.generation == previous.generation && verified.statementSHA256 != previous.statementSHA256)) reject()
            }
            persist(it, candidate, raw)
            verified
        }
    }

    fun <T> useCurrent(now: ULong, action: (PluginReleaseSignature.VerifiedChannelPolicy) -> T): T {
        val owner = PluginReleaseCatalog.open(directory) ?: reject()
        return owner.use {
            val raw = it.readChannelRecord(name) ?: reject()
            val record = decode(raw)
            if (now < record.observed) reject()
            val verified = verify(record, now)
            persist(it, record.copy(observed = now), raw)
            action(verified)
        }
    }

    private fun verify(record: Record, now: ULong) =
        PluginReleaseSignature.verifyChannelPolicy(record.statement, record.signature, record.policy, channel, root, now)

    private fun persist(owner: PluginReleaseCatalog, record: Record, previous: ByteArray?) {
        val raw = ByteBuffer.allocate(HEADER_SIZE + record.statement.size + record.policy.size)
            .put(1).put(rootDigest).putLong(record.observed.toLong()).putInt(record.statement.size)
            .put(record.signature).put(record.statement).put(record.policy).array()
        if (!owner.writeChannelRecord(name, raw, previous)) throw PluginReleaseCatalog.Rejected(PluginReleaseCatalog.Reason.IO)
    }

    private fun decode(raw: ByteArray): Record {
        if (raw.size < HEADER_SIZE) reject()
        val input = ByteBuffer.wrap(raw)
        if (input.get().toInt() != 1 || !ByteArray(64).also { input.get(it) }.contentEquals(rootDigest)) reject()
        val observed = input.long.toULong()
        val statementSize = input.int
        val policySize = input.remaining() - 64 - statementSize
        if (statementSize !in 1..65536 || policySize !in 1..131072 || observed == 0uL) reject()
        val signature = ByteArray(64).also { input.get(it) }
        val statement = ByteArray(statementSize).also { input.get(it) }
        val policy = ByteArray(policySize).also { input.get(it) }
        return Record(observed, statement, signature, policy)
    }

    private fun reject(): Nothing = throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.CHANNEL_HISTORY)

    private companion object {
        const val HEADER_SIZE = 1 + 64 + 8 + 4 + 64
    }
}
