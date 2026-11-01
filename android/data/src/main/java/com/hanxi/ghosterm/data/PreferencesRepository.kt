package com.hanxi.ghosterm.data

import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

sealed interface PreferencesState {
    data object Loading : PreferencesState
    data class Ready(val values: AppPreferences, val issues: Set<PreferenceIssue>, val generation: Long) : PreferencesState
    /** Keep the last known value for display; consumers must not apply a fallback on a read failure. */
    data class Unavailable(val message: String, val previous: AppPreferences?) : PreferencesState
}

sealed interface PreferenceWrite {
    data object Saved : PreferenceWrite
    /** The storage contract guarantees the previous document is unchanged. */
    data object Unchanged : PreferenceWrite
    /** A publication/durability error requires reload, never a guessed success or rollback. */
    data object Unknown : PreferenceWrite
}

interface PreferencesStorage {
    /** Only a genuinely absent file returns null. Access and format errors must throw. */
    fun read(): ByteArray?
    /** Compare against the loaded bytes before publication; this method must classify exceptions. */
    fun write(expected: ByteArray?, bytes: ByteArray): PreferenceWrite
}

sealed interface PreferenceUpdate {
    data class Saved(val state: PreferencesState.Ready) : PreferenceUpdate
    data class Failed(val message: String, val needsReload: Boolean = false) : PreferenceUpdate
}

/** One app-owned repository. Every edit is transformed against the latest serialized commit. */
class PreferencesRepository(
    private val storage: PreferencesStorage,
    private val dispatcher: CoroutineDispatcher = Dispatchers.IO,
) {
    private val mutex = Mutex()
    private val mutable = MutableStateFlow<PreferencesState>(PreferencesState.Loading)
    val state: StateFlow<PreferencesState> = mutable.asStateFlow()
    private var original: ByteArray? = null
    private var generation = 0L

    suspend fun reload(): PreferencesState = withContext(dispatcher) {
        mutex.withLock {
            val previous = when (val value = mutable.value) {
                is PreferencesState.Ready -> value.values
                is PreferencesState.Unavailable -> value.previous
                PreferencesState.Loading -> null
            }
            try {
                val bytes = storage.read()
                val decoded = bytes?.let(PreferencesCodec::decode) ?: DecodedPreferences(AppPreferences())
                original = bytes?.copyOf()
                generation++
                PreferencesState.Ready(decoded.values, decoded.issues, generation).also { mutable.value = it }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (_: Exception) {
                PreferencesState.Unavailable("无法读取偏好，请重试；原文件没有被重建。", previous).also { mutable.value = it }
            }
        }
    }

    suspend fun update(change: (AppPreferences) -> AppPreferences): PreferenceUpdate = withContext(dispatcher) {
        mutex.withLock {
            val ready = mutable.value as? PreferencesState.Ready
                ?: return@withLock PreferenceUpdate.Failed("偏好尚未成功载入，请重新载入。", true)
            val candidate = try { change(ready.values) } catch (_: IllegalArgumentException) {
                return@withLock PreferenceUpdate.Failed("偏好值无效，尚未保存。")
            }
            val encoded = PreferencesCodec.encode(candidate)
            // A storage implementation must classify post-publication failures.
            // If it unexpectedly throws, ownership is unknown, so block writes until reload.
            val outcome = try { storage.write(original?.copyOf(), encoded.copyOf()) }
                catch (cancelled: CancellationException) {
                    mutable.value = PreferencesState.Unavailable("保存结果尚未确认，请重新载入后检查。", ready.values)
                    throw cancelled
                }
                catch (_: Exception) { PreferenceWrite.Unknown }
            when (outcome) {
                PreferenceWrite.Saved -> {
                    original = encoded
                    generation++
                    val committed = PreferencesState.Ready(candidate, emptySet(), generation)
                    mutable.value = committed
                    PreferenceUpdate.Saved(committed)
                }
                PreferenceWrite.Unchanged -> PreferenceUpdate.Failed("偏好未保存，原设置保持不变。")
                PreferenceWrite.Unknown -> {
                    mutable.value = PreferencesState.Unavailable("保存结果尚未确认，请重新载入后检查。", ready.values)
                    PreferenceUpdate.Failed("保存结果尚未确认，请重新载入后检查。", true)
                }
            }
        }
    }

    /** UI confirms first; this transaction changes preferences only. */
    suspend fun reset(): PreferenceUpdate = update { AppPreferences() }
}
