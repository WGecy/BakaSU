package org.bakasu.bakasu.ui.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.bakasu.bakasu.data.AppSettingsRepository
import org.bakasu.bakasu.data.shell.KsuCliRepository

data class IoBlockDevice(
    val name: String,
    val current: String,
    val available: List<String>,
)

data class IoSchedulerUiState(
    val devices: List<IoBlockDevice> = emptyList(),
    val current: String = "",
    val available: List<String> = emptyList(),
    val loaded: Boolean = false,
    val pinned: String? = null,
)

class IoSchedulerViewModel(
    private val ksuCli: KsuCliRepository,
    private val settingsRepository: AppSettingsRepository,
) : ViewModel() {
    private val mutableState = MutableStateFlow(IoSchedulerUiState())
    val uiState: StateFlow<IoSchedulerUiState> = mutableState.asStateFlow()

    suspend fun refresh() {
        withContext(Dispatchers.IO) {
            val devices = readBlockDevices()
            val primary = devices.firstOrNull { it.name == "sda" } ?: devices.firstOrNull()
            val pinned = settingsRepository.getString(PREF_KEY)
            var current = primary?.current.orEmpty()
            if (pinned != null && primary != null && current.isNotBlank() && current != pinned) {
                if (applyToAll(devices, pinned)) {
                    current = readCurrent(primary.name) ?: pinned
                }
            }
            mutableState.value = IoSchedulerUiState(
                devices = devices,
                current = current,
                available = primary?.available.orEmpty(),
                loaded = true,
                pinned = pinned,
            )
        }
    }

    fun setScheduler(name: String) {
        viewModelScope.launch {
            val pinned = mutableState.value.pinned
            val ok = withContext(Dispatchers.IO) {
                if (pinned == null) {
                    applyToAll(mutableState.value.devices, name)
                } else {
                    ksuCli.execKsud("iosched apply $name")
                }
            }
            if (!ok) return@launch
            if (pinned != null) persist(name)
            refresh()
        }
    }

    fun setPinned(pinned: Boolean) {
        viewModelScope.launch {
            val current = mutableState.value.current
            if (pinned && current.isBlank()) return@launch
            val ok = withContext(Dispatchers.IO) {
                if (pinned) ksuCli.execKsud("iosched apply $current") else ksuCli.execKsud("iosched clear")
            }
            if (!ok) return@launch
            persist(current.takeIf { pinned })
            refresh()
        }
    }

    private fun persist(name: String?) {
        settingsRepository.putString(PREF_KEY, name)
    }

    private suspend fun applyToAll(devices: List<IoBlockDevice>, name: String): Boolean {
        var applied = false
        for (dev in devices) {
            ksuCli.exec("echo $name > /sys/block/${dev.name}/queue/scheduler")
            if (readCurrent(dev.name) == name) applied = true
        }
        return applied
    }

    private suspend fun readCurrent(name: String): String? = ksuCli.exec("cat /sys/block/$name/queue/scheduler")?.let(::parseCurrent)

    private fun parseCurrent(raw: String): String? = raw.substringAfter('[', "").substringBefore(']').trim().ifBlank { null }

    private suspend fun readBlockDevices(): List<IoBlockDevice> {
        val blocks = ksuCli.exec("ls /sys/block/")
            ?.split("\n")
            ?.map { it.trim() }
            ?.filter { it.startsWith("sd") || it.startsWith("nvme") }
            ?: return emptyList()
        return blocks.mapNotNull { name ->
            val sched = ksuCli.exec("cat /sys/block/$name/queue/scheduler") ?: return@mapNotNull null
            val available = sched.split(" ")
                .map { it.trim('[', ']') }
                .filter { it.isNotBlank() && it != "none" }
            if (available.isEmpty()) {
                null
            } else {
                IoBlockDevice(
                    name = name,
                    current = parseCurrent(sched).orEmpty(),
                    available = available,
                )
            }
        }
    }

    companion object {
        const val PREF_KEY = "io_scheduler"
    }
}
