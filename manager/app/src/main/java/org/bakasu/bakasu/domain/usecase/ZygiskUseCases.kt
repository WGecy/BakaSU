package org.bakasu.bakasu.domain.usecase

import kotlinx.coroutines.flow.StateFlow
import org.bakasu.bakasu.data.kernel.ZygiskRepository
import org.bakasu.bakasu.domain.model.ZygiskState

class ObserveZygiskStateUseCase(private val repository: ZygiskRepository) {
    operator fun invoke(): StateFlow<ZygiskState> = repository.state
}

class RefreshZygiskStateUseCase(private val repository: ZygiskRepository) {
    suspend operator fun invoke(): Result<Unit> = repository.refresh()
}

class SetZygiskEnabledUseCase(private val repository: ZygiskRepository) {
    suspend operator fun invoke(enabled: Boolean): Result<Unit> = repository.setEnabled(enabled)
}

class ReloadZygiskModulesUseCase(private val repository: ZygiskRepository) {
    suspend operator fun invoke(): Result<Unit> = repository.reloadRuntime()
}

class RefreshZygiskSnapshotUseCase(private val repository: ZygiskRepository) {
    suspend operator fun invoke(): Result<Unit> = repository.refreshSnapshot()
}

class EnsureZygiskDaemonUseCase(private val repository: ZygiskRepository) {
    suspend operator fun invoke(): Result<Unit> = repository.ensureDaemon()
}

class SetZygiskConfigUseCase(private val repository: ZygiskRepository) {
    suspend operator fun invoke(key: String, value: String): Result<Unit> = repository.setConfig(key, value)
}
