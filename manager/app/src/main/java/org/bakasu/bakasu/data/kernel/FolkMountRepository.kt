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
import org.bakasu.bakasu.domain.model.FolkMountMode
import org.bakasu.bakasu.domain.model.FolkMountState
import org.bakasu.bakasu.domain.model.FolkMountStatus
import org.bakasu.bakasu.domain.model.NomountStatus
import org.json.JSONObject

class FolkMountRepository(
    private val ksuCliRepository: KsuCliRepository,
    private val context: Context,
) {
    private val mutex = Mutex()
    private val mutableState = MutableStateFlow(FolkMountState())
    val state: StateFlow<FolkMountState> = mutableState.asStateFlow()

    suspend fun refresh(): Result<Unit> = mutex.withLock {
        withContext(Dispatchers.IO) { reload() }
    }

    suspend fun setMode(mode: FolkMountMode): Result<Unit> = mutex.withLock {
        withContext(Dispatchers.IO) {
            mutableState.update { it.copy(isWriting = true) }
            val outcome = runCatching {
                ksuCliRepository.execMount("set-mode ${mode.token}").getOrThrow()
            }
            mutableState.update { it.copy(isWriting = false) }
            reload()
            outcome.map { }
        }
    }

    private suspend fun reload(): Result<Unit> = runCatching {
        val raw = ksuCliRepository.execMount("status --json").getOrThrow()
        val status = parseStatus(raw)
        val nomount = readNomountStatus().getOrNull()
        mutableState.update {
            it.copy(
                status = status,
                nomountStatus = nomount ?: it.nomountStatus,
                nomountReadError = nomount == null,
                isLoading = false,
                isWriting = false,
                readFailed = false,
                error = null,
            )
        }
    }.onFailure { error ->
        mutableState.update {
            it.copy(
                isLoading = false,
                isWriting = false,
                readFailed = true,
                nomountReadError = true,
                error = error.message ?: error.toString(),
            )
        }
    }

    suspend fun getNomountStatus(): Result<NomountStatus> = withContext(Dispatchers.IO) {
        readNomountStatus()
    }

    private suspend fun readNomountStatus(): Result<NomountStatus> = runCatching {
        val result = ksuCliRepository.execKsudResult("nomount status --json")
        check(result.isSuccess) { "ksud nomount status failed (exit ${result.code})" }
        val payload = result.out.firstOrNull { it.isNotBlank() }
            ?: error("ksud nomount status returned no payload")
        NomountStatus.fromJson(JSONObject(payload))
    }

    suspend fun setNomountIsolated(enabled: Boolean): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            val state = if (enabled) "on" else "off"
            check(ksuCliRepository.execKsud("nomount uid isolated $state", true)) {
                "ksud nomount uid isolated $state failed"
            }
        }
    }

    suspend fun injectNomountRules(): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            check(ksuCliRepository.execKsud("nomount inject", true)) { "ksud nomount inject failed" }
        }
    }

    suspend fun clearNomountRules(): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            check(ksuCliRepository.execKsud("nomount clear", true)) { "ksud nomount clear failed" }
        }
    }

    suspend fun clearNomountEverything(): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            check(ksuCliRepository.execKsud("nomount clear", true)) { "ksud nomount clear failed" }
            check(ksuCliRepository.execKsud("nomount uid set", true)) {
                "ksud nomount uid set failed"
            }
        }
    }

    suspend fun setNomountUids(uids: List<Int>): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            val list = uids.distinct().sorted().joinToString(" ")
            val command = if (list.isEmpty()) "nomount uid set" else "nomount uid set $list"
            check(ksuCliRepository.execKsud(command, true)) { "ksud nomount uid set failed" }
        }
    }

    suspend fun hotLoadNomountModule(id: String): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            check(
                ksuCliRepository.execKsud(
                    "nomount hot-load ${ksuCliRepository.shellQuote(id)}",
                    true,
                ),
            ) { "ksud nomount hot-load failed" }
        }
    }

    suspend fun hotUnloadNomountModule(id: String): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            check(
                ksuCliRepository.execKsud(
                    "nomount hot-unload ${ksuCliRepository.shellQuote(id)}",
                    true,
                ),
            ) { "ksud nomount hot-unload failed" }
        }
    }

    suspend fun setModuleSkip(id: String, skip: Boolean): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            val flag = if (skip) "" else " --off"
            check(
                ksuCliRepository.execKsud(
                    "module skip ${ksuCliRepository.shellQuote(id)}$flag",
                    true,
                ),
            ) { "ksud module skip failed" }
        }
    }

    private fun parseStatus(raw: String): FolkMountStatus {
        val trimmed = raw.trim()
        val start = trimmed.indexOf('{')
        val end = trimmed.lastIndexOf('}')
        if (start < 0 || end <= start) {
            throw IllegalStateException(
                trimmed.ifBlank { context.getString(R.string.folk_mount_no_data) },
            )
        }
        val json = runCatching { JSONObject(trimmed.substring(start, end + 1)) }.getOrNull()
            ?: throw IllegalStateException(context.getString(R.string.folk_mount_no_data))

        val schema = if (json.has("schema_version") && !json.isNull("schema_version")) {
            json.optInt("schema_version", 0)
        } else {
            null
        }
        if (
            schema != null &&
            schema !in FolkMountStatus.OLDEST_SCHEMA_VERSION..FolkMountStatus.SCHEMA_VERSION
        ) {
            throw IllegalStateException(
                context.getString(R.string.folk_mount_unsupported_schema, schema),
            )
        }
        return runCatching { FolkMountStatus.fromJson(json) }.getOrElse { error ->
            val message = error.message.orEmpty()
            throw IllegalStateException(
                if (message.contains("configured_mode")) {
                    context.getString(R.string.folk_mount_no_mode)
                } else {
                    message.takeIf { it.isNotBlank() }
                        ?: context.getString(R.string.folk_mount_no_data)
                },
            )
        }
    }
}
