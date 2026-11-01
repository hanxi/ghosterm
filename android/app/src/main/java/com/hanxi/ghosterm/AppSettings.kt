package com.hanxi.ghosterm

import android.app.Application
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.AppPreferences
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.*
import java.io.Closeable
import java.util.UUID

data class SettingsProblem(val client: UUID, val message: String, val needsReload: Boolean)
data class AppSettingsState(val storage: PreferencesState = PreferencesState.Loading,
    val committed: AppPreferences = AppPreferences())
sealed interface SettingsReload {
    data class Loaded(val state: PreferencesState) : SettingsReload
    data class Rejected(val message: String) : SettingsReload
}

/** One process owner; queued operations contain values, never an Activity or View. */
class AppSettings(
    private val repository: PreferencesRepository,
    scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate),
) : Closeable {
    private val lifetime = SupervisorJob(scope.coroutineContext[Job])
    private val owner = CoroutineScope(scope.coroutineContext + lifetime)
    private val published = MutableStateFlow(AppSettingsState())
    val state: StateFlow<AppSettingsState> = published.asStateFlow()
    private var generation = -1L
    private val problem = MutableStateFlow<SettingsProblem?>(null)
    val lastProblem: StateFlow<SettingsProblem?> = problem.asStateFlow()
    private val pending = MutableStateFlow(0)
    val pendingCount: StateFlow<Int> = pending.asStateFlow()
    private sealed interface Operation {
        val client: UUID
        fun cancel()
        data class Save(override val client: UUID, val edits: List<PreferenceEdit>, val reset: Boolean,
            val result: CompletableDeferred<PreferenceUpdate>) : Operation { override fun cancel() { result.cancel() } }
        data class Reload(override val client: UUID, val result: CompletableDeferred<SettingsReload>) : Operation { override fun cancel() { result.cancel() } }
    }
    private val requests = Channel<Operation>(MAX_QUEUED)
    init {
        owner.launch {
            repository.state.collect(::publish)
        }
        owner.launch {
            try {
                publish(repository.reload())
                for (operation in requests) {
                    try {
                        when (operation) {
                            is Operation.Save -> {
                                val result = if (operation.reset) repository.reset()
                                    else repository.update { before -> operation.edits.fold(before) { p, edit -> edit.apply(p) } }
                                publish(repository.state.value)
                                if (result is PreferenceUpdate.Failed)
                                    problem.value = SettingsProblem(operation.client, result.message, result.needsReload)
                                else if (problem.value?.client == operation.client) problem.value = null
                                operation.result.complete(result)
                            }
                            is Operation.Reload -> {
                                val state = repository.reload()
                                publish(state)
                                if (state is PreferencesState.Ready && problem.value?.client == operation.client) problem.value = null
                                operation.result.complete(SettingsReload.Loaded(state))
                            }
                        }
                    } catch (cancelled: CancellationException) {
                        operation.cancel(); throw cancelled
                    } catch (_: Exception) {
                        val message = "设置操作结果尚未确认，请重新载入。"
                        problem.value = SettingsProblem(operation.client, message, true)
                        when (operation) {
                            is Operation.Save -> operation.result.complete(PreferenceUpdate.Failed(message, true))
                            is Operation.Reload -> operation.result.complete(SettingsReload.Rejected(message))
                        }
                    } finally { pending.update { it - 1 } }
                }
            } finally {
                requests.close()
                while (true) {
                    val queued = requests.tryReceive().getOrNull() ?: break
                    queued.cancel(); pending.update { it - 1 }
                }
            }
        }
    }
    private fun publish(storage: PreferencesState) {
        if (storage !== repository.state.value) return
        var committed = published.value.committed
        when (storage) {
            is PreferencesState.Ready -> if (storage.generation >= generation) {
                generation = storage.generation; committed = storage.values
            }
            is PreferencesState.Unavailable -> storage.previous?.let { committed = it }
            PreferencesState.Loading -> Unit
        }
        published.value = AppSettingsState(storage, committed)
    }
    private fun accept(operation: Operation): Boolean {
        if (!lifetime.isActive) return false
        pending.update { it + 1 }
        if (requests.trySend(operation).isSuccess) return true
        pending.update { it - 1 }; return false
    }
    suspend fun save(client: UUID, edits: List<PreferenceEdit>, reset: Boolean = false): PreferenceUpdate {
        val result = CompletableDeferred<PreferenceUpdate>()
        if (!accept(Operation.Save(client, edits.toList(), reset, result)))
            return PreferenceUpdate.Failed("保存请求较多或服务已结束，候选值尚未保存，请重试。")
        // Cancelling a page's await does not cancel the app-owned operation.
        return result.await()
    }
    suspend fun reload(client: UUID): SettingsReload {
        val result = CompletableDeferred<SettingsReload>()
        if (!accept(Operation.Reload(client, result))) return SettingsReload.Rejected("暂时无法重新载入，请重试。")
        return result.await()
    }
    override fun close() {
        requests.close(); lifetime.cancel()
        while (true) {
            val queued = requests.tryReceive().getOrNull() ?: break
            queued.cancel(); pending.update { it - 1 }
        }
    }
    suspend fun awaitClosed() { lifetime.join() }
    companion object {
        internal const val MAX_QUEUED = 64
        fun forApplication(application: Application): AppSettings =
            (application as? GhostermApplication)?.settings ?: error("应用未配置设置所有者")
    }
}
