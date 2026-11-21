package org.bakasu.bakasu.domain.model

data class ZygiskProcess(
    val name: String,
    val pid: Int,
)

data class ZygoteAbiStatus(
    val abi: String,
    val status: String,
)

data class BuiltinZygiskStatus(
    val enabled: Boolean = false,
    val monitorRunning: Boolean = false,
    val daemonRunning: Boolean = false,
    val zygoteInjected: Boolean = false,
    val fullyActive: Boolean = false,
    val zygotes: List<ZygoteAbiStatus> = emptyList(),
    val monitor: ZygiskProcess? = null,
    val daemons: List<ZygiskProcess> = emptyList(),
    val version: String = "",
    val versionCode: String = "",
)

data class BuiltinZygiskConfig(
    val anonymousMemory: Boolean = false,
    val customLinker: Boolean = false,
    val pltEngine: String = PLT_ENGINE_YUKIPLT,
) {
    companion object {
        const val PLT_ENGINE_YUKIPLT = "yukiplt"
        const val PLT_ENGINE_LSPLT = "lsplt"
    }
}

data class InjectedZygiskProcess(
    val name: String,
    val pid: Int,
    val kind: String,
)

data class InjectedZygiskModule(
    val id: String,
    val name: String,
    val version: String,
    val type: String,
    val abi: List<String> = emptyList(),
    val targets: List<String> = emptyList(),
    val companion: Boolean = false,
    val state: String = STATE_UNKNOWN,
    val processes: List<InjectedZygiskProcess> = emptyList(),
) {
    companion object {
        const val STATE_INJECTED = "injected"
        const val STATE_LOADED = "loaded"
        const val STATE_FAILED = "failed"
        const val STATE_UNKNOWN = "unknown"
    }
}

data class ZygiskImplementationInfo(
    val state: String = STATE_NONE,
    val id: String = "",
    val name: String = "",
    val version: String = "",
    val providers: List<String> = emptyList(),
) {
    companion object {
        const val STATE_RUNNING = "running"
        const val STATE_REBOOT = "reboot"
        const val STATE_DISABLED = "disabled"
        const val STATE_NONE = "none"
    }
}
