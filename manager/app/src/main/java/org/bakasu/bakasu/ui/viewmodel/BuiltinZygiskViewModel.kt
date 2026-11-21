package org.bakasu.bakasu.ui.viewmodel

import android.content.Context
import androidx.compose.runtime.Immutable
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.bakasu.bakasu.R
import org.bakasu.bakasu.data.module.ModuleRepository
import org.bakasu.bakasu.data.shell.KsuCliRepository
import org.bakasu.bakasu.domain.model.BuiltinZygiskConfig
import org.bakasu.bakasu.domain.model.BuiltinZygiskStatus
import org.bakasu.bakasu.domain.model.InjectedZygiskModule
import org.bakasu.bakasu.domain.model.InstalledModule
import org.bakasu.bakasu.domain.model.ZygiskImplementationInfo
import org.bakasu.bakasu.domain.usecase.RebootUseCase

@Immutable
data class BuiltinZygiskUiState(
    val isLoading: Boolean = true,
    val isBusy: Boolean = false,
    val status: BuiltinZygiskStatus = BuiltinZygiskStatus(),
    val implementation: ZygiskImplementationInfo = ZygiskImplementationInfo(),
    val config: BuiltinZygiskConfig = BuiltinZygiskConfig(),
    val injectedModules: List<InjectedZygiskModule> = emptyList(),
    val providerModules: List<InstalledModule> = emptyList(),
)

sealed interface BuiltinZygiskUiEvent {
    data class Message(val message: String) : BuiltinZygiskUiEvent

    data class Switched(val targetName: String) : BuiltinZygiskUiEvent
}

class BuiltinZygiskViewModel(
    private val context: Context,
    private val ksuCliRepository: KsuCliRepository,
    private val moduleRepository: ModuleRepository,
    private val rebootUseCase: RebootUseCase,
) : ViewModel() {
    private val mutableState = MutableStateFlow(BuiltinZygiskUiState())
    val uiState: StateFlow<BuiltinZygiskUiState> = mutableState.asStateFlow()

    private val mutableEvents = MutableSharedFlow<BuiltinZygiskUiEvent>(extraBufferCapacity = 4)
    val events: SharedFlow<BuiltinZygiskUiEvent> = mutableEvents.asSharedFlow()

    init {
        refresh()
    }

    fun refresh() {
        if (mutableState.value.isBusy) return
        viewModelScope.launch {
            mutableState.update { it.copy(isLoading = true) }
            reload()
        }
    }

    fun rearm() {
        if (mutableState.value.isBusy) return
        mutableState.update { it.copy(isBusy = true) }
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { ksuCliRepository.rearmBuiltinZygisk() }
            if (!ok) {
                mutableEvents.emit(BuiltinZygiskUiEvent.Message(context.getString(R.string.builtin_zygisk_rearm_failed)))
            }
            reload()
        }
    }

    fun setEnabled(enabled: Boolean) {
        if (mutableState.value.isBusy) return
        mutableState.update { it.copy(isBusy = true) }
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { ksuCliRepository.setBuiltinZygiskEnabled(enabled) }
            if (!ok) {
                mutableEvents.emit(BuiltinZygiskUiEvent.Message(context.getString(R.string.builtin_zygisk_toggle_failed)))
            }
            reload()
        }
    }

    fun softReboot() {
        viewModelScope.launch {
            withContext(Dispatchers.IO) { rebootUseCase("soft_reboot") }
        }
    }

    fun setOption(key: String, value: String) {
        if (mutableState.value.isBusy) return
        if (mutableState.value.config.value(key) == value) return
        mutableState.update { it.copy(isBusy = true) }
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { ksuCliRepository.setBuiltinZygiskConfig(key, value) }
            if (!ok) {
                mutableEvents.emit(BuiltinZygiskUiEvent.Message(context.getString(R.string.builtin_zygisk_option_failed)))
            }
            reload()
        }
    }

    fun switchTo(target: String, targetName: String) {
        if (mutableState.value.isBusy) return
        mutableState.update { it.copy(isBusy = true) }
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { ksuCliRepository.switchZygiskImplementation(target) }
            if (ok) {
                mutableEvents.emit(BuiltinZygiskUiEvent.Switched(targetName))
            } else {
                mutableEvents.emit(BuiltinZygiskUiEvent.Message(context.getString(R.string.builtin_zygisk_switch_failed_message)))
            }
            reload()
        }
    }

    private fun BuiltinZygiskConfig.value(key: String): String = when (key) {
        OPTION_ANONYMOUS_MEMORY -> if (anonymousMemory) "1" else "0"
        OPTION_CUSTOM_LINKER -> if (customLinker) "1" else "0"
        OPTION_PLT_ENGINE -> pltEngine
        else -> ""
    }

    private suspend fun reload() {
        val status = withContext(Dispatchers.IO) { ksuCliRepository.getBuiltinZygiskStatus() }
        val implementation = withContext(Dispatchers.IO) { ksuCliRepository.getZygiskImplementationInfo() }
        val config = withContext(Dispatchers.IO) { ksuCliRepository.getBuiltinZygiskConfig() }
        val injectedModules = withContext(Dispatchers.IO) { ksuCliRepository.getInjectedZygiskModules() }
        moduleRepository.refreshInstalledModules(manual = false, checkUpdates = false)
        val providerModules = moduleRepository.installedModules.value.modules
            .filter { it.zygisk || it.suppressedByBuiltin }
            .sortedBy { it.name }
        mutableState.value = BuiltinZygiskUiState(
            isLoading = false,
            isBusy = false,
            status = status,
            implementation = implementation,
            config = config,
            injectedModules = injectedModules,
            providerModules = providerModules,
        )
    }

    companion object {
        const val OPTION_ANONYMOUS_MEMORY = "anonymous_memory"
        const val OPTION_CUSTOM_LINKER = "custom_linker"
        const val OPTION_PLT_ENGINE = "plt_engine"
    }
}
