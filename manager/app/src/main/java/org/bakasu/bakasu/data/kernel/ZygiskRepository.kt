package org.bakasu.bakasu.data.kernel

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.bakasu.bakasu.data.shell.KsuCliRepository
import org.bakasu.bakasu.domain.model.ZygiskConfig
import org.bakasu.bakasu.domain.model.ZygiskRuntimeRecord
import org.bakasu.bakasu.domain.model.ZygiskState
import org.json.JSONObject

/**
 * 内置 YukiZygisk 的数据仓库.
 *
 * 所有状态都来自 `ksud zygisk status --json`, 写操作先改内核/配置再整表重读, 保证界面与内核一致.
 */
class ZygiskRepository(
    private val ksuCliRepository: KsuCliRepository,
) {
    private val mutex = Mutex()

    private val mutableState = MutableStateFlow(ZygiskState())

    val state: StateFlow<ZygiskState> = mutableState.asStateFlow()

    suspend fun refresh(): Result<Unit> = mutex.withLock {
        withContext(Dispatchers.IO) {
            mutableState.update { it.copy(isRefreshing = !it.isLoading) }
            reload()
        }
    }

    suspend fun setEnabled(enabled: Boolean): Result<Unit> = submit {
        ksuCliRepository.execZygisk(if (enabled) "enable" else "disable")
    }.map { }

    suspend fun reloadRuntime(): Result<Unit> = submit {
        ksuCliRepository.execZygisk("reload")
    }.map { }

    suspend fun refreshSnapshot(): Result<Unit> = submit {
        ksuCliRepository.execZygisk("refresh-snapshot")
    }.map { }

    suspend fun ensureDaemon(): Result<Unit> = submit {
        ksuCliRepository.execZygisk("ensure-daemon --abi all")
    }.map { }

    suspend fun setConfig(key: String, value: String): Result<Unit> = submit {
        ksuCliRepository.execZygisk("config set ${quote(key)} ${quote(value)}")
    }.map { }

    suspend fun setConfig(pairs: List<Pair<String, String>>): Result<Unit> = submit {
        val args = pairs.joinToString(" ") { (key, value) -> "${quote(key)} ${quote(value)}" }
        ksuCliRepository.execZygisk("config set $args")
    }.map { }

    private suspend fun reload(): Result<Unit> = runCatching {
        val raw = ksuCliRepository.execZygisk("status --json").getOrThrow()
        mutableState.value = parseStatus(raw)
    }.onFailure { error ->
        mutableState.update {
            it.copy(
                isLoading = false,
                isRefreshing = false,
                error = error.message ?: error.toString(),
            )
        }
    }

    private suspend fun submit(command: suspend () -> Result<String>): Result<String> = mutex.withLock {
        withContext(Dispatchers.IO) {
            mutableState.update { it.copy(isSubmitting = true) }
            val outcome = runCatching { command().getOrThrow() }
            mutableState.update { it.copy(isSubmitting = false) }
            reload()
            outcome
        }
    }

    private fun parseStatus(raw: String): ZygiskState {
        val trimmed = raw.trim()
        val start = trimmed.indexOf('{')
        val end = trimmed.lastIndexOf('}')
        if (start < 0 || end <= start) {
            return ZygiskState(isLoading = false, error = trimmed.ifBlank { "YukiZygisk 内核未返回数据" })
        }
        val json = JSONObject(trimmed.substring(start, end + 1))
        val config = json.optJSONObject("config")
        val recordsJson = json.optJSONArray("runtime")
        val records = buildList {
            if (recordsJson != null) {
                for (index in 0 until recordsJson.length()) {
                    val item = recordsJson.optJSONObject(index) ?: continue
                    add(
                        ZygiskRuntimeRecord(
                            pid = item.optInt("pid"),
                            generation = item.optInt("generation"),
                            restarts = item.optInt("restarts"),
                            kind = item.optString("kind"),
                            state = item.optString("state"),
                            injectionState = item.optString("injection_state"),
                            abi = item.optString("abi"),
                            targetType = item.optString("target_type"),
                            flags = item.optInt("flags"),
                            process = item.optString("process"),
                            target = item.optString("target"),
                            module = item.optString("module"),
                        ),
                    )
                }
            }
        }
        // supported 必须来自内核: 内核没编入 YukiZygisk 时 ksud 会返回 false,
        // 只有 GET_RUNTIME 之类能力缺失时 runtime 才为空但 supported 仍为 true。
        val runtimeError = json.optString("runtime_error").takeIf { it.isNotBlank() }
        return ZygiskState(
            supported = json.optBoolean("supported"),
            enabled = json.optBoolean("enabled"),
            generation = json.optInt("generation"),
            safeMode = json.optBoolean("safe_mode"),
            zygoteCrashes = json.optInt("zygote_crashes"),
            safeModeZygote = json.optString("safe_mode_zygote"),
            capabilities = json.optInt("capabilities"),
            config = ZygiskConfig(
                yukilinker = config?.optBoolean("yukilinker") ?: true,
                anonymousMemory = config?.optBoolean("anonymous_memory") ?: true,
                earlyLoad = config?.optBoolean("early_load") ?: false,
                denylistMode = config?.optInt("denylist_mode") ?: 0,
                dmesgLog = config?.optBoolean("dmesg_log") ?: false,
                crashProtection = config?.optBoolean("crash_protection") ?: true,
            ),
            records = records,
            error = runtimeError,
            isLoading = false,
            isRefreshing = false,
        )
    }

    private fun quote(value: String) = "'" + value.replace("'", "'\"'\"'") + "'"
}
