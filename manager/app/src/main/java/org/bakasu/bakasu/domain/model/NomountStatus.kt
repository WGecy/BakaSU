package org.bakasu.bakasu.domain.model

import androidx.compose.runtime.Immutable
import org.json.JSONObject

enum class NomountDriverState(val token: String) {
    ABSENT("absent"),
    OUTDATED("outdated"),
    READY("ready"),
    ;

    companion object {
        fun fromToken(token: String?): NomountDriverState? = entries.firstOrNull { it.token == token }
    }
}

@Immutable
data class NomountModule(
    val id: String,
    val rules: Int,
)

@Immutable
data class NomountStatus(
    val driverState: NomountDriverState,
    val driverVersion: String?,
    val apiOk: Boolean,
    val rulesExpected: Int,
    val rulesApplied: Int,
    val kernelRules: Int,
    val modules: List<NomountModule>,
    val uids: List<Int>,
    val uidsActive: List<Int>,
    val deferredTo: String?,
    val ownsInjection: Boolean,
    val dataVerified: String?,
    val dataError: String?,
    val driverKind: String?,
    val unavailableCode: String?,
    val unavailableDetail: String?,
    val uidCount: Int,
    val isolatedBlocked: Boolean,
    val error: String?,
) {
    val injected: Boolean
        get() = apiOk && rulesApplied >= rulesExpected && rulesExpected > 0 && dataVerified != null && dataError == null

    companion object {
        const val SCHEMA_VERSION = 2
        private const val OLDEST_SCHEMA_VERSION = 1

        fun fromJson(json: JSONObject): NomountStatus {
            val schema = json.optInt("schema_version", -1)
            check(schema in OLDEST_SCHEMA_VERSION..SCHEMA_VERSION) {
                "unsupported nomount schema_version $schema"
            }
            val state = NomountDriverState.fromToken(json.optStringOrNull("driver_state"))
                ?: error("nomount status has no valid driver_state")
            val modules = json.optJSONArray("modules")?.let { array ->
                List(array.length()) { index ->
                    val entry = array.optJSONObject(index) ?: error("invalid module entry")
                    NomountModule(
                        id = entry.optStringOrNull("id") ?: error("module entry without id"),
                        rules = entry.optInt("rules", 0),
                    )
                }
            }.orEmpty()
            return NomountStatus(
                driverState = state,
                driverVersion = json.optStringOrNull("driver_version"),
                apiOk = json.optBoolean("api_ok", false),
                rulesExpected = json.optInt("rules_expected", 0),
                rulesApplied = json.optInt("rules_applied", 0),
                kernelRules = json.optInt("kernel_rules", json.optInt("rules_applied", 0)),
                modules = modules,
                uids = json.optIntList("uids"),
                uidsActive = json.optIntList("uids_active"),
                deferredTo = json.optStringOrNull("deferred_to"),
                ownsInjection = json.optBoolean("owns_injection", false),
                dataVerified = json.optStringOrNull("data_verified"),
                dataError = json.optStringOrNull("data_error"),
                driverKind = json.optStringOrNull("driver_kind"),
                unavailableCode = json.optStringOrNull("unavailable_code"),
                unavailableDetail = json.optStringOrNull("unavailable_detail"),
                uidCount = json.optInt("uid_count", 0),
                isolatedBlocked = json.optBoolean("isolated_blocked", false),
                error = json.optStringOrNull("error"),
            )
        }
    }
}

private fun JSONObject.optIntList(key: String): List<Int> {
    val array = optJSONArray(key) ?: return emptyList()
    return List(array.length()) { index -> array.optInt(index, 0) }
}
