package org.bakasu.bakasu.domain.usecase

import kotlinx.coroutines.flow.StateFlow
import org.bakasu.bakasu.data.kernel.FolkMountRepository
import org.bakasu.bakasu.domain.model.FolkMountMode
import org.bakasu.bakasu.domain.model.FolkMountState

class ObserveFolkMountStateUseCase(private val repository: FolkMountRepository) {
    operator fun invoke(): StateFlow<FolkMountState> = repository.state
}

class RefreshFolkMountStateUseCase(private val repository: FolkMountRepository) {
    suspend operator fun invoke() = repository.refresh()
}

class SetFolkMountModeUseCase(private val repository: FolkMountRepository) {
    suspend operator fun invoke(mode: FolkMountMode) = repository.setMode(mode)
}
