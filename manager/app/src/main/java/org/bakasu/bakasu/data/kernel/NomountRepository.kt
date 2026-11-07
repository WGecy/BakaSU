package org.bakasu.bakasu.data.kernel

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.bakasu.bakasu.R
import org.bakasu.bakasu.data.shell.KsuCliRepository
import org.bakasu.bakasu.domain.model.NomountModule
import org.bakasu.bakasu.domain.model.NomountRule
import org.bakasu.bakasu.domain.model.NomountState
import org.json.JSONObject

/**
 * 内核 NoMount 管理器的数据仓库。
 *
 * 所有读取都走一次聚合的 ksud nomount snapshot 调用, 所有写入走对应的子命令,
 * 写入完成后立刻重新拉取快照, 保证界面展示的是内核里的真实状态。
 */
class NomountRepository(
    private val ksuCliRepository: KsuCliRepository,
    private val context: Context,
) {
    private val mutex = Mutex()
    private val mutableState = MutableStateFlow(NomountState())
    val state: StateFlow<NomountState> = mutableState.asStateFlow()

    suspend fun refresh(): Result<Unit> = mutex.withLock {
        withContext(Dispatchers.IO) {
            mutableState.update { it.copy(isRefreshing = !it.isLoading) }
            reload()
        }
    }

    suspend fun setEnabled(enabled: Boolean): Result<Unit> = run("set-enabled ${if (enabled) 1 else 0}")

    suspend fun setSafeMode(enabled: Boolean): Result<Unit> = run("safe-mode ${if (enabled) "on" else "off"}")

    suspend fun setBlockIsolated(enabled: Boolean): Result<Unit> = run("isolated ${if (enabled) "on" else "off"}")

    suspend fun inject(): Result<Unit> = run("inject")

    suspend fun clearRules(): Result<Unit> = run("clear-rules")

    suspend fun loadModule(moduleId: String): Result<Int> = submit { ksuCliRepository.execNomount("load ${quote(moduleId)}") }
        .map { parseCount(it, "loaded") }

    suspend fun unloadModule(moduleId: String): Result<Int> = submit { ksuCliRepository.execNomount("unload ${quote(moduleId)}") }
        .map { parseCount(it, "unloaded") }

    suspend fun setModuleDisabled(moduleId: String, disabled: Boolean): Result<Unit> = run("module-disable ${quote(moduleId)} ${if (disabled) 1 else 0}")

    suspend fun setModuleSkipMount(moduleId: String, skip: Boolean): Result<Unit> = submit {
        if (ksuCliRepository.setModuleSkipMount(moduleId, skip)) {
            Result.success("ok")
        } else {
            Result.failure(
                IllegalStateException(context.getString(R.string.nomount_skip_mount_failed, moduleId)),
            )
        }
    }.map { }

    suspend fun addRule(
        virtualPath: String,
        realPath: String?,
        whiteout: Boolean,
        opaque: Boolean,
    ): Result<Unit> = run(
        buildString {
            append("add ")
            append(quote(virtualPath))
            if (!whiteout) {
                append(' ')
                append(quote(realPath.orEmpty()))
            }
            if (whiteout) append(" --whiteout")
            if (opaque) append(" --opaque")
        },
    )

    suspend fun removeRules(virtualPaths: List<String>): Result<Unit> {
        if (virtualPaths.isEmpty()) return Result.success(Unit)
        return run("remove ${virtualPaths.joinToString(" ") { quote(it) }}")
    }

    suspend fun addExclusion(uid: Int): Result<Unit> = run("exclude-add $uid")

    suspend fun removeExclusion(uid: Int): Result<Unit> = run("exclude-remove $uid")

    /** 执行一条 no-arg 输出的 nomount 子命令。 */
    private suspend fun run(args: String): Result<Unit> = submit { ksuCliRepository.execNomount(args) }.map { }

    /** 在已持有 [mutex] 的情况下重新加载快照。 */
    private suspend fun reload(): Result<Unit> = runCatching {
        val raw = ksuCliRepository.execNomount("snapshot").getOrThrow()
        mutableState.value = parseSnapshot(raw)
    }.onFailure { error ->
        mutableState.update {
            it.copy(
                isLoading = false,
                isRefreshing = false,
                error = error.message ?: error.toString(),
            )
        }
    }

    /** 执行写入命令, 无论成败都重新拉取快照以便界面回到真实状态。 */
    private suspend fun submit(command: suspend () -> Result<String>): Result<String> = mutex.withLock {
        withContext(Dispatchers.IO) {
            mutableState.update { it.copy(isSubmitting = true) }
            val outcome = runCatching { command().getOrThrow() }
            mutableState.update { it.copy(isSubmitting = false) }
            reload()
            outcome
        }
    }

    private fun parseSnapshot(raw: String): NomountState {
        val trimmed = raw.trim()
        val start = trimmed.indexOf('{')
        val end = trimmed.lastIndexOf('}')
        if (start < 0 || end <= start) {
            val message = context.getString(R.string.nomount_no_data)
            return NomountState(isLoading = false, error = trimmed.ifBlank { message })
        }
        val json = JSONObject(trimmed.substring(start, end + 1))
        return NomountState(
            supported = json.optBoolean("supported", false),
            enabled = json.optBoolean("enabled", false),
            safeMode = json.optBoolean("safe_mode", false),
            isolatedBlocked = json.optBoolean("isolated_blocked", false),
            version = json.optInt("version", 0),
            activeModuleCount = json.optInt("active_module_count", 0),
            modules = parseModules(json),
            rules = parseRules(json),
            exclusions = parseExclusions(json),
            error = json.optString("error").takeIf { it.isNotBlank() },
            isLoading = false,
            isRefreshing = false,
        )
    }

    private fun parseModules(json: JSONObject): List<NomountModule> {
        val array = json.optJSONArray("modules") ?: return emptyList()
        return (0 until array.length()).mapNotNull { index ->
            val item = array.optJSONObject(index) ?: return@mapNotNull null
            NomountModule(
                id = item.optString("id"),
                name = item.optString("name"),
                version = item.optString("version"),
                author = item.optString("author"),
                description = item.optString("description"),
                disabled = item.optBoolean("disabled", false),
                skipMount = item.optBoolean("skip_mount", false),
                fileCount = item.optInt("file_count", 0),
                loaded = item.optInt("loaded", 0),
            )
        }
    }

    private fun parseRules(json: JSONObject): List<NomountRule> {
        val array = json.optJSONArray("rules") ?: return emptyList()
        return (0 until array.length()).mapNotNull { index ->
            val item = array.optJSONObject(index) ?: return@mapNotNull null
            NomountRule(
                virtualPath = item.optString("virtual"),
                realPath = item.optString("real"),
                flags = item.optInt("flags", 0),
                whiteout = item.optBoolean("whiteout", false),
                opaque = item.optBoolean("opaque", false),
                virtualDir = item.optBoolean("virtual_dir", false),
                isDir = item.optBoolean("is_dir", false),
                uid = item.optInt("uid", 0),
                uidEnd = item.optInt("uid_end", 0),
                denyStart = item.optInt("deny_start", 0),
                denyEnd = item.optInt("deny_end", 0),
            )
        }
    }

    private fun parseExclusions(json: JSONObject): List<Int> {
        val array = json.optJSONArray("exclusions") ?: return emptyList()
        return (0 until array.length()).map { index -> array.optInt(index, 0) }.filter { it > 0 }
    }

    private fun parseCount(raw: String, key: String): Int {
        val line = raw.lineSequence().firstOrNull { it.contains(key) } ?: raw
        return line.substringAfter(':', line).trim().toIntOrNull() ?: 0
    }

    private fun quote(value: String): String = "'" + value.replace("'", "'\"'\"'") + "'"
}
