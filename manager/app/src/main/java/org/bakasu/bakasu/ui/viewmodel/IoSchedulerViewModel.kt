package org.bakasu.bakasu.ui.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
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
    val error: String? = null,
)

class IoSchedulerViewModel(
    private val ksuCli: KsuCliRepository,
    private val settingsRepository: AppSettingsRepository,
) : ViewModel() {
    private val mutableState = MutableStateFlow(IoSchedulerUiState())
    val uiState: StateFlow<IoSchedulerUiState> = mutableState.asStateFlow()

    /** 仿 SUSFS: suspend 可等待 — 页面级 pageLoaded 在真实完成后置位 */
    suspend fun refresh() {
        withContext(Dispatchers.IO) {
            val devices = readBlockDevices()
            val primary = devices.firstOrNull { it.name == "sda" }
                ?: devices.firstOrNull()
            val pinned = settingsRepository.getString(PREF_KEY)
            val current = primary?.current.orEmpty()
            // 事件驱动同步: 固化开着且被外部覆盖(厂商 init 等) → 自动纠正 (用户打开工具箱即生效)
            if (pinned != null && current.isNotBlank() && current != pinned) {
                restoreScheduler(pinned)
            }
            mutableState.value = IoSchedulerUiState(
                devices = devices,
                current = primary?.current.orEmpty(),
                available = primary?.available.orEmpty(),
                loaded = true,
                pinned = pinned,
            )
        }
    }

    private fun restoreScheduler(name: String) {
        var success = false
        for (dev in mutableState.value.devices) {
            ksuCli.exec("echo $name > /sys/block/${dev.name}/queue/scheduler")
            val cur = ksuCli.exec("cat /sys/block/${dev.name}/queue/scheduler").orEmpty()
            if (cur.contains("[$name]")) success = true
        }
        if (success) {
            android.util.Log.i("IoSchedulerVM", "打开时同步: 已纠正为 $name")
        }
    }

    fun setScheduler(name: String) {
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) {
                // echo 写 sysfs 无输出 (exec 返回 null), 必须写后读回验证真实状态
                var success = false
                for (dev in mutableState.value.devices) {
                    ksuCli.exec("echo $name > /sys/block/${dev.name}/queue/scheduler")
                    val cur = ksuCli.exec("cat /sys/block/${dev.name}/queue/scheduler").orEmpty()
                    if (cur.contains("[$name]")) {
                        success = true
                    }
                }
                success
            }
            if (ok) {
                if (mutableState.value.pinned != null) {
                    settingsRepository.putString(PREF_KEY, name)
                    ksuCli.exec("echo $name > $FLAG_FILE")
                }
                refresh()
            } else {
                mutableState.update { it.copy(error = "切换失败") }
            }
        }
    }

    companion object {
        const val PREF_KEY = "io_scheduler"
        const val FLAG_FILE = "/data/adb/ksu/io_scheduler"
    }

    /** 固化当前调度器 (开机自动应用 — ksud 开机读标志文件) */
    fun pinCurrent() {
        val current = mutableState.value.current
        if (current.isBlank()) return
        settingsRepository.putString(PREF_KEY, current)
        // ksud 开机应用 (Greezer 会拦 App 广播, 必须走 ksud 侧)
        ksuCli.exec("mkdir -p /data/adb/ksu && echo $current > $FLAG_FILE")
        mutableState.update { it.copy(pinned = current) }
    }

    /** 取消固化 (开机回默认) */
    fun unpin() {
        settingsRepository.putString(PREF_KEY, null)
        ksuCli.exec("rm -f $FLAG_FILE")
        mutableState.update { it.copy(pinned = null) }
    }

    private fun readBlockDevices(): List<IoBlockDevice> {
        val blocks = ksuCli.exec("ls /sys/block/")
            ?.split("\n")
            ?.map { it.trim() }
            ?.filter { it.startsWith("sd") || it.startsWith("nvme") }
            ?: return emptyList()
        return blocks.mapNotNull { name ->
            val sched = ksuCli.exec("cat /sys/block/$name/queue/scheduler") ?: return@mapNotNull null
            // 输出如: none mq-deadline [adios] kyber bfq cpq
            val available = sched.split(" ")
                .map { it.trim('[', ']') }
                .filter { it.isNotBlank() && it != "none" }
            val current = sched.substringAfter('[', "").substringBefore(']').trim()
            if (available.isEmpty()) {
                null
            } else {
                IoBlockDevice(name = name, current = current, available = available)
            }
        }
    }
}
