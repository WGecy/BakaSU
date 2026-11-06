package org.bakasu.bakasu.ui.viewmodel

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import org.bakasu.bakasu.R
import org.bakasu.bakasu.domain.model.NomountState
import org.bakasu.bakasu.domain.usecase.AddNomountExclusionUseCase
import org.bakasu.bakasu.domain.usecase.AddNomountRuleUseCase
import org.bakasu.bakasu.domain.usecase.ClearNomountRulesUseCase
import org.bakasu.bakasu.domain.usecase.InjectNomountModulesUseCase
import org.bakasu.bakasu.domain.usecase.LoadNomountModuleUseCase
import org.bakasu.bakasu.domain.usecase.ObserveNomountStateUseCase
import org.bakasu.bakasu.domain.usecase.RefreshNomountStateUseCase
import org.bakasu.bakasu.domain.usecase.RemoveNomountExclusionUseCase
import org.bakasu.bakasu.domain.usecase.RemoveNomountRuleUseCase
import org.bakasu.bakasu.domain.usecase.SetNomountEnabledUseCase
import org.bakasu.bakasu.domain.usecase.SetNomountIsolatedUseCase
import org.bakasu.bakasu.domain.usecase.SetNomountModuleDisabledUseCase
import org.bakasu.bakasu.domain.usecase.SetNomountModuleSkipMountUseCase
import org.bakasu.bakasu.domain.usecase.SetNomountSafeModeUseCase
import org.bakasu.bakasu.domain.usecase.UnloadNomountModuleUseCase

sealed interface NomountUiEvent {
    data class Message(val message: String) : NomountUiEvent
}

/**
 * 工具箱 → NoMount 管理器的状态持有者。
 *
 * 所有操作完成后都会由仓库重新拉取内核快照, 因此界面不需要做乐观更新。
 */
class NomountViewModel(
    private val context: Context,
    observeState: ObserveNomountStateUseCase,
    private val refreshState: RefreshNomountStateUseCase,
    private val setEnabledUseCase: SetNomountEnabledUseCase,
    private val setSafeModeUseCase: SetNomountSafeModeUseCase,
    private val setBlockIsolatedUseCase: SetNomountIsolatedUseCase,
    private val injectModulesUseCase: InjectNomountModulesUseCase,
    private val clearRulesUseCase: ClearNomountRulesUseCase,
    private val loadModuleUseCase: LoadNomountModuleUseCase,
    private val unloadModuleUseCase: UnloadNomountModuleUseCase,
    private val setModuleDisabledUseCase: SetNomountModuleDisabledUseCase,
    private val setModuleSkipMountUseCase: SetNomountModuleSkipMountUseCase,
    private val addRuleUseCase: AddNomountRuleUseCase,
    private val removeRuleUseCase: RemoveNomountRuleUseCase,
    private val addExclusionUseCase: AddNomountExclusionUseCase,
    private val removeExclusionUseCase: RemoveNomountExclusionUseCase,
) : ViewModel() {
    val uiState: StateFlow<NomountState> = observeState()

    private val mutableEvents = MutableSharedFlow<NomountUiEvent>(extraBufferCapacity = 1)
    val events = mutableEvents

    suspend fun refresh() {
        refreshState()
    }

    fun setEnabled(enabled: Boolean) {
        submit({ setEnabledUseCase(enabled) }, R.string.nomount_msg_enabled)
    }

    fun setSafeMode(enabled: Boolean) {
        submit({ setSafeModeUseCase(enabled) }, R.string.nomount_msg_safe_mode)
    }

    fun setBlockIsolated(enabled: Boolean) {
        submit({ setBlockIsolatedUseCase(enabled) }, R.string.nomount_msg_isolated)
    }

    fun inject() {
        submit({ injectModulesUseCase() }, R.string.nomount_msg_injected)
    }

    fun clearRules() {
        submit({ clearRulesUseCase() }, R.string.nomount_msg_rules_cleared)
    }

    fun loadModule(moduleId: String) {
        submit({ loadModuleUseCase(moduleId).map { } }, R.string.nomount_msg_module_loaded)
    }

    fun unloadModule(moduleId: String) {
        submit({ unloadModuleUseCase(moduleId).map { } }, R.string.nomount_msg_module_unloaded)
    }

    fun setModuleDisabled(moduleId: String, disabled: Boolean) {
        submit(
            { setModuleDisabledUseCase(moduleId, disabled) },
            R.string.nomount_msg_module_disabled,
        )
    }

    fun setModuleSkipMount(moduleId: String, skip: Boolean) {
        submit(
            { setModuleSkipMountUseCase(moduleId, skip) },
            R.string.nomount_msg_module_skip,
        )
    }

    fun addRule(virtualPath: String, realPath: String?, whiteout: Boolean, opaque: Boolean) {
        submit(
            { addRuleUseCase(virtualPath, realPath, whiteout, opaque) },
            R.string.nomount_msg_rule_added,
        )
    }

    fun removeRule(virtualPath: String) {
        submit({ removeRuleUseCase(virtualPath) }, R.string.nomount_msg_rule_removed)
    }

    fun addExclusion(uid: Int) {
        submit({ addExclusionUseCase(uid) }, R.string.nomount_msg_exclusion_added)
    }

    fun removeExclusion(uid: Int) {
        submit({ removeExclusionUseCase(uid) }, R.string.nomount_msg_exclusion_removed)
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
            mutableEvents.emit(NomountUiEvent.Message(message))
        }
    }
}
