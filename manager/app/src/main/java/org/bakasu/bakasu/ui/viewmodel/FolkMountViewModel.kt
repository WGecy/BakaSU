package org.bakasu.bakasu.ui.viewmodel

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.launch
import org.bakasu.bakasu.R
import org.bakasu.bakasu.domain.model.FolkMountMode
import org.bakasu.bakasu.domain.model.FolkMountState
import org.bakasu.bakasu.domain.usecase.ObserveFolkMountStateUseCase
import org.bakasu.bakasu.domain.usecase.RefreshFolkMountStateUseCase
import org.bakasu.bakasu.domain.usecase.SetFolkMountModeUseCase

class FolkMountViewModel(
    private val context: Context,
    observeState: ObserveFolkMountStateUseCase,
    private val refreshState: RefreshFolkMountStateUseCase,
    private val setModeUseCase: SetFolkMountModeUseCase,
) : ViewModel() {
    val uiState: StateFlow<FolkMountState> = observeState()

    private val mutableEvents = MutableSharedFlow<String>(extraBufferCapacity = 1)
    val events: SharedFlow<String> = mutableEvents.asSharedFlow()

    suspend fun refresh() {
        refreshState()
    }

    fun setMode(mode: FolkMountMode) {
        viewModelScope.launch {
            val result = setModeUseCase(mode)
            val message = if (result.isSuccess) {
                context.getString(R.string.folk_mount_msg_mode_changed)
            } else {
                result.exceptionOrNull()?.message?.takeIf { it.isNotBlank() }
                    ?: context.getString(R.string.settings_folk_mount_write_failed)
            }
            mutableEvents.emit(message)
        }
    }
}
