package org.bakasu.bakasu.domain.usecase

import kotlinx.coroutines.flow.StateFlow
import org.bakasu.bakasu.data.kernel.NomountRepository
import org.bakasu.bakasu.domain.model.NomountState

class ObserveNomountStateUseCase(private val repository: NomountRepository) {
    operator fun invoke(): StateFlow<NomountState> = repository.state
}

class RefreshNomountStateUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke() = repository.refresh()
}

class SetNomountEnabledUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke(enabled: Boolean) = repository.setEnabled(enabled)
}

class SetNomountSafeModeUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke(enabled: Boolean) = repository.setSafeMode(enabled)
}

class SetNomountIsolatedUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke(enabled: Boolean) = repository.setBlockIsolated(enabled)
}

class InjectNomountModulesUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke() = repository.inject()
}

class ClearNomountRulesUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke() = repository.clearRules()
}

class LoadNomountModuleUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke(moduleId: String) = repository.loadModule(moduleId)
}

class UnloadNomountModuleUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke(moduleId: String) = repository.unloadModule(moduleId)
}

class SetNomountModuleDisabledUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke(moduleId: String, disabled: Boolean) = repository.setModuleDisabled(moduleId, disabled)
}

class SetNomountModuleSkipMountUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke(moduleId: String, skip: Boolean) = repository.setModuleSkipMount(moduleId, skip)
}

class AddNomountRuleUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke(
        virtualPath: String,
        realPath: String?,
        whiteout: Boolean,
        opaque: Boolean,
    ) = repository.addRule(virtualPath, realPath, whiteout, opaque)
}

class RemoveNomountRuleUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke(virtualPath: String) = repository.removeRules(listOf(virtualPath))
}

class AddNomountExclusionUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke(uid: Int) = repository.addExclusion(uid)
}

class RemoveNomountExclusionUseCase(private val repository: NomountRepository) {
    suspend operator fun invoke(uid: Int) = repository.removeExclusion(uid)
}
