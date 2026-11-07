package org.bakasu.bakasu.ui.viewmodel

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import org.bakasu.bakasu.R
import org.bakasu.bakasu.domain.model.ZygiskState
import org.bakasu.bakasu.domain.usecase.EnsureZygiskDaemonUseCase
import org.bakasu.bakasu.domain.usecase.ObserveZygiskStateUseCase
import org.bakasu.bakasu.domain.usecase.RefreshZygiskSnapshotUseCase
import org.bakasu.bakasu.domain.usecase.RefreshZygiskStateUseCase
import org.bakasu.bakasu.domain.usecase.ReloadZygiskModulesUseCase
import org.bakasu.bakasu.domain.usecase.SetZygiskConfigUseCase
import org.bakasu.bakasu.domain.usecase.SetZygiskEnabledUseCase

sealed interface ZygiskUiEvent {
    data class Message(val message: String) : ZygiskUiEvent
}

class ZygiskViewModel(
    private val context: Context,
    observeState: ObserveZygiskStateUseCase,
    private val refreshState: RefreshZygiskStateUseCase,
    private val setEnabled: SetZygiskEnabledUseCase,
    private val reloadModules: ReloadZygiskModulesUseCase,
    private val refreshSnapshot: RefreshZygiskSnapshotUseCase,
    private val ensureDaemon: EnsureZygiskDaemonUseCase,
    private val setConfig: SetZygiskConfigUseCase,
) : ViewModel() {
    val uiState: StateFlow<ZygiskState> = observeState()

    private val mutableEvents = MutableSharedFlow<ZygiskUiEvent>(extraBufferCapacity = 1)

    val events = mutableEvents

    suspend fun refresh() {
        refreshState()
    }

    fun toggleEnabled(enabled: Boolean) {
        submit({ setEnabled(enabled) }, if (enabled) R.string.zygisk_msg_enabled else R.string.zygisk_msg_disabled)
    }

    fun reload() {
        submit({ reloadModules() }, R.string.zygisk_msg_reloaded)
    }

    fun refreshEarlySnapshot() {
        submit({ refreshSnapshot() }, R.string.zygisk_msg_snapshot_refreshed)
    }

    fun startDaemon() {
        submit({ ensureDaemon() }, R.string.zygisk_msg_daemon_ensured)
    }

    fun setSwitchOption(key: String, value: Boolean) {
        submit({ setConfig(key, value.toString()) }, R.string.zygisk_msg_config_saved)
    }

    fun setDenylistMode(mode: Int) {
        submit({ setConfig("denylist_mode", mode.toString()) }, R.string.zygisk_msg_config_saved)
    }

    private fun submit(command: suspend () -> Result<Unit>, successMessage: Int) {
        viewModelScope.launch {
            val result = command()
            val message = if (result.isSuccess) {
                context.getString(successMessage)
            } else {
                result.exceptionOrNull()?.message?.takeIf { it.isNotBlank() }
                    ?: context.getString(R.string.operation_failed)
            }
            mutableEvents.emit(ZygiskUiEvent.Message(message))
        }
    }
}
