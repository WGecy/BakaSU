package org.bakasu.bakasu.domain.model

import androidx.compose.runtime.Immutable
import org.json.JSONObject

enum class FolkMountMode(val token: String) {
    AUTO("auto"),
    BUILTIN("builtin"),
    METAMODULE("metamodule"),
    NOMOUNT("nomount"),
    OFF("off"),
    ;

    companion object {
        fun fromToken(token: String?): FolkMountMode? = entries.firstOrNull { it.token == token }
    }
}

enum class FolkMountProvider(val token: String) {
    BUILTIN("builtin"),
    METAMODULE("metamodule"),
    NOMOUNT("nomount"),
    NONE("none"),
    ;

    companion object {
        fun fromToken(token: String?): FolkMountProvider? = entries.firstOrNull { it.token == token }
    }
}

@Immutable
data class FolkMountStatus(
    val configuredMode: FolkMountMode,
    val nextProvider: FolkMountProvider?,
    val metamoduleId: String?,
    val metamoduleEnabled: Boolean,
    val bootId: String?,
    val bootProvider: FolkMountProvider?,
    val bootResult: String,
    val targetCount: Int?,
    val bootWarning: String?,
    val bootWarningCode: String?,
    val error: String?,
) {
    fun shouldWrite(mode: FolkMountMode): Boolean = configuredMode != mode ||
        (mode == FolkMountMode.NOMOUNT && bootProvider != null && bootProvider != FolkMountProvider.NOMOUNT)

    companion object {
        const val SCHEMA_VERSION = 2
        internal const val OLDEST_SCHEMA_VERSION = 1

        fun fromJson(json: JSONObject): FolkMountStatus {
            val schema = json.optInt("schema_version", -1)
            check(schema in OLDEST_SCHEMA_VERSION..SCHEMA_VERSION) {
                "unsupported folk mount schema_version $schema"
            }
            val configuredMode = FolkMountMode.fromToken(json.optStringOrNull("configured_mode"))
                ?: error("folk mount status has no valid configured_mode")
            return FolkMountStatus(
                configuredMode = configuredMode,
                nextProvider = FolkMountProvider.fromToken(json.optStringOrNull("next_provider")),
                metamoduleId = json.optStringOrNull("metamodule_id"),
                metamoduleEnabled = json.optBoolean("metamodule_enabled", false),
                bootId = json.optStringOrNull("boot_id"),
                bootProvider = FolkMountProvider.fromToken(json.optStringOrNull("boot_provider")),
                bootResult = json.optStringOrNull("boot_result").orEmpty(),
                targetCount = json.optIntOrNull("target_count"),
                bootWarning = json.optStringOrNull("boot_warning"),
                bootWarningCode = json.optStringOrNull("boot_warning_code"),
                error = json.optStringOrNull("error"),
            )
        }
    }
}

data class FolkMountState(
    val status: FolkMountStatus? = null,
    val nomountStatus: NomountStatus? = null,
    val nomountReadError: Boolean = false,
    val isLoading: Boolean = true,
    val isWriting: Boolean = false,
    val readFailed: Boolean = false,
    val error: String? = null,
)

internal fun JSONObject.optStringOrNull(key: String): String? {
    if (!has(key) || isNull(key)) return null
    return optString(key).takeIf { it.isNotEmpty() }
}

internal fun JSONObject.optIntOrNull(key: String): Int? {
    if (!has(key) || isNull(key)) return null
    return optInt(key)
}
