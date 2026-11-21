package org.bakasu.bakasu.ui.component

import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import org.bakasu.bakasu.R
import org.bakasu.bakasu.domain.model.NomountDriverState
import org.bakasu.bakasu.domain.model.NomountStatus

@Composable
fun nomountDriverStateLabel(state: NomountDriverState): String = stringResource(
    when (state) {
        NomountDriverState.ABSENT -> R.string.nomount_driver_absent
        NomountDriverState.OUTDATED -> R.string.nomount_driver_outdated
        NomountDriverState.READY -> R.string.nomount_driver_ready
    },
)

@Composable
fun nomountUnavailableReason(status: NomountStatus): String? {
    val detail = status.unavailableDetail.orEmpty()
    return when (status.unavailableCode) {
        "no_asset_for_kmi" -> stringResource(R.string.nomount_unavailable_no_asset, detail)
        "outdated" -> stringResource(R.string.nomount_unavailable_outdated, detail)
        "not_loaded" -> stringResource(R.string.nomount_unavailable_not_loaded)
        "unknown_kmi" -> stringResource(R.string.nomount_unavailable_unknown_kmi)
        else -> null
    }
}
