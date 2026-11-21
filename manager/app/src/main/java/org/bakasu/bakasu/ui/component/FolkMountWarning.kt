package org.bakasu.bakasu.ui.component

import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import org.bakasu.bakasu.R
import org.bakasu.bakasu.domain.model.FolkMountStatus

@Composable
fun folkMountWarningText(status: FolkMountStatus): String? {
    val code = status.bootWarningCode ?: return status.bootWarning
    val res = when (code) {
        "nomount_ui_fallback" -> R.string.settings_folk_mount_nomount_ui_fallback
        "nomount_ui_unconfirmed" -> R.string.settings_folk_mount_nomount_ui_unconfirmed
        "nomount_driver_unavailable" -> R.string.settings_folk_mount_nomount_unavailable_hint
        "mount_step_interrupted" -> R.string.settings_folk_mount_step_interrupted
        else -> return status.bootWarning
    }
    return stringResource(res)
}
