package org.bakasu.bakasu.ui.viewmodel

import androidx.compose.runtime.Immutable
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.bakasu.bakasu.data.kernel.FolkMountRepository
import org.bakasu.bakasu.data.packageinfo.InstalledPackageRepository
import org.bakasu.bakasu.data.profile.ProfileRepository
import org.bakasu.bakasu.domain.model.FolkMountMode
import org.bakasu.bakasu.domain.model.FolkMountStatus
import org.bakasu.bakasu.domain.model.NomountStatus

@Immutable
data class ExclusionApp(
    val uid: Int,
    val label: String,
    val packageName: String,
    val isSystem: Boolean,
    val excluded: Boolean,
)

@Immutable
data class NomountUiState(
    val isLoading: Boolean = true,
    val isWriting: Boolean = false,
    val status: NomountStatus? = null,
    val folkMountStatus: FolkMountStatus? = null,
    val readError: Boolean = false,
    val actionError: Boolean = false,
    val apps: List<ExclusionApp> = emptyList(),
    val appsLoading: Boolean = true,
    val showSystemApps: Boolean = false,
)

class NomountViewModel(
    private val folkMountRepository: FolkMountRepository,
    private val installedPackageRepository: InstalledPackageRepository,
    private val profileRepository: ProfileRepository,
) : ViewModel() {
    private val mutableState = MutableStateFlow(NomountUiState())
    val uiState: StateFlow<NomountUiState> = mutableState.asStateFlow()

    init {
        refresh()
    }

    fun refresh() {
        if (mutableState.value.isWriting) return
        mutableState.update { it.copy(isLoading = true, actionError = false) }
        viewModelScope.launch { refreshStatuses() }
    }

    fun enableNomount() {
        if (mutableState.value.isWriting) return
        mutableState.update { it.copy(isWriting = true, actionError = false) }
        viewModelScope.launch {
            val result = folkMountRepository.setMode(FolkMountMode.NOMOUNT)
            refreshStatuses(actionError = result.isFailure)
        }
    }

    private suspend fun refreshStatuses(actionError: Boolean? = null) {
        val statusResult = folkMountRepository.getNomountStatus()
        folkMountRepository.refresh()
        val mountStatus = folkMountRepository.state.value.status
        mutableState.update {
            it.copy(
                isLoading = false,
                isWriting = false,
                status = statusResult.getOrNull() ?: it.status,
                folkMountStatus = mountStatus ?: it.folkMountStatus,
                readError = statusResult.isFailure,
                actionError = actionError ?: it.actionError,
            )
        }
    }

    fun injectRules() = runAction { folkMountRepository.injectNomountRules() }

    fun clearRules() = runAction { folkMountRepository.clearNomountRules() }

    fun clearEverything() = runAction { folkMountRepository.clearNomountEverything() }

    fun setIsolated(enabled: Boolean) = runAction { folkMountRepository.setNomountIsolated(enabled) }

    fun hotLoad(id: String) = runAction { folkMountRepository.hotLoadNomountModule(id) }

    fun hotUnload(id: String) = runAction { folkMountRepository.hotUnloadNomountModule(id) }

    fun setSkip(id: String, skip: Boolean) = runAction {
        folkMountRepository.setModuleSkip(id, skip).also { result ->
            if (result.isSuccess) folkMountRepository.injectNomountRules()
        }
    }

    fun setShowSystemApps(show: Boolean) {
        mutableState.update { it.copy(showSystemApps = show) }
    }

    fun loadApps() {
        if (mutableState.value.apps.isNotEmpty() || !mutableState.value.appsLoading) return
        viewModelScope.launch {
            val apps = loadExclusionApps()
            mutableState.update { it.copy(apps = apps, appsLoading = false) }
        }
    }

    fun saveExclusions(uids: List<Int>) = runAction { folkMountRepository.setNomountUids(uids) }

    fun clearExclusions() = runAction { folkMountRepository.setNomountUids(emptyList()) }

    suspend fun profileExcludedUids(includeSystem: Boolean): List<Int> = withContext(Dispatchers.IO) {
        mutableState.value.apps
            .filter { app ->
                (includeSystem || !app.isSystem) &&
                    runCatching { profileRepository.uidShouldUmount(app.uid) }
                        .getOrDefault(false)
            }
            .map { app -> app.uid }
    }

    private fun runAction(block: suspend () -> Result<Unit>) {
        if (mutableState.value.isWriting) return
        mutableState.update { it.copy(isWriting = true, actionError = false) }
        viewModelScope.launch {
            val result = block()
            val refreshed = folkMountRepository.getNomountStatus()
            mutableState.update {
                it.copy(
                    isWriting = false,
                    actionError = result.isFailure,
                    status = refreshed.getOrNull() ?: it.status,
                    readError = refreshed.isFailure,
                )
            }
        }
    }

    private suspend fun loadExclusionApps(): List<ExclusionApp> = withContext(Dispatchers.IO) {
        val excluded = mutableState.value.status?.uids.orEmpty().toSet()
        val cached = installedPackageRepository.packages.value
        val packages = if (cached.isEmpty()) {
            installedPackageRepository.refresh()
            installedPackageRepository.packages.value
        } else {
            cached
        }
        packages
            .groupBy { it.uid }
            .map { (uid, group) ->
                val primary = group.first()
                ExclusionApp(
                    uid = uid,
                    label = primary.appLabel.ifBlank { primary.packageName },
                    packageName = primary.packageName,
                    isSystem = group.any { it.isSystem },
                    excluded = uid in excluded,
                )
            }
            .sortedBy { it.label.lowercase() }
    }
}

internal fun shouldShowNomountSetup(
    status: NomountStatus?,
    mount: FolkMountStatus?,
): Boolean = mount != null &&
    status != null &&
    status.deferredTo == null &&
    (mount.configuredMode != FolkMountMode.NOMOUNT || mount.shouldWrite(FolkMountMode.NOMOUNT))
