package org.bakasu.bakasu.domain.model

/** yzconfig.json 中的六项可调参数, 与内核侧 uapi/yukizygisk.h 的 load flags 对应。 */
data class ZygiskConfig(
    val yukilinker: Boolean = true,
    val anonymousMemory: Boolean = true,
    val earlyLoad: Boolean = false,
    val denylistMode: Int = 0,
    val dmesgLog: Boolean = false,
    val crashProtection: Boolean = true,
)

/** 内核 yz_runtime_record 的界面映射 (一个被注入/跳过的进程)。 */
data class ZygiskRuntimeRecord(
    val pid: Int,
    val generation: Int,
    val restarts: Int,
    val kind: String,
    val state: String,
    val injectionState: String,
    val abi: String,
    val targetType: String,
    val flags: Int,
    val process: String,
    val target: String,
    val module: String,
) {
    val isInjected: Boolean get() = injectionState == "injected"

    val isFailed: Boolean get() = injectionState == "failed" || injectionState == "skipped"
}

/** 工具箱 → YukiZygisk 管理器的完整界面状态。 */
data class ZygiskState(
    val supported: Boolean = false,
    val enabled: Boolean = false,
    val generation: Int = 0,
    val safeMode: Boolean = false,
    val zygoteCrashes: Int = 0,
    val safeModeZygote: String = "",
    val capabilities: Int = 0,
    val config: ZygiskConfig = ZygiskConfig(),
    val records: List<ZygiskRuntimeRecord> = emptyList(),
    val error: String? = null,
    val isLoading: Boolean = true,
    val isRefreshing: Boolean = false,
    val isSubmitting: Boolean = false,
) {
    /** 已成功注入的进程数, 概览统计用。 */
    val activeCount: Int get() = records.count { it.isInjected }
}
