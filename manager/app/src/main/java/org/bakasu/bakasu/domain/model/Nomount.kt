package org.bakasu.bakasu.domain.model

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
}

data class NomountRule(
    val virtualPath: String,
    val realPath: String,
    val whiteout: Boolean,
    val opaque: Boolean,
    val virtualDir: Boolean,
    val uid: Int,
    val uidEnd: Int,
) {
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
    val isSubmitting: Boolean = false,
)
