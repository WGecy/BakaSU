package org.bakasu.bakasu.domain.model

/**
 * 单个已安装模块在 NoMount 中的状态。
 */
data class NomountModule(
    val id: String,
    val name: String,
    val version: String,
    val author: String,
    val description: String,
    val disabled: Boolean,
    val skipMount: Boolean,
    val fileCount: Int,
    val loaded: Int,
) {
    val isLoaded: Boolean get() = loaded > 0

    val isActive: Boolean get() = !disabled && !skipMount && loaded > 0
}

/**
 * 内核 NoMount 驱动中的一条规则。
 *
 * [whiteout] 表示隐藏目标路径, [opaque] 表示接管目录但保留原有内容,
 * 其余情况是将 [virtualPath] 重定向到 [realPath]。
 */
data class NomountRule(
    val virtualPath: String,
    val realPath: String,
    val flags: Int,
    val whiteout: Boolean,
    val opaque: Boolean,
    val virtualDir: Boolean,
    val isDir: Boolean,
    val uid: Int,
    val uidEnd: Int,
    val denyStart: Int,
    val denyEnd: Int,
) {
    /** 仅对特定 UID 生效的规则, 而不是全局默认规则。 */
    val hasUidScope: Boolean get() = uid != 0 || uidEnd != 0
}

data class NomountState(
    val supported: Boolean = false,
    val enabled: Boolean = false,
    val safeMode: Boolean = false,
    val isolatedBlocked: Boolean = false,
    val version: Int = 0,
    val activeModuleCount: Int = 0,
    val modules: List<NomountModule> = emptyList(),
    val rules: List<NomountRule> = emptyList(),
    val exclusions: List<Int> = emptyList(),
    val error: String? = null,
    val isLoading: Boolean = true,
    val isRefreshing: Boolean = false,
    val isSubmitting: Boolean = false,
)
