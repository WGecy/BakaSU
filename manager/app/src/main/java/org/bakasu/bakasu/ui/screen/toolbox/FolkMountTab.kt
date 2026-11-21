package org.bakasu.bakasu.ui.screen.toolbox

import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.twotone.Layers
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.res.stringResource
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import org.bakasu.bakasu.R
import org.bakasu.bakasu.domain.model.FolkMountMode
import org.bakasu.bakasu.domain.model.FolkMountProvider
import org.bakasu.bakasu.domain.model.FolkMountStatus
import org.bakasu.bakasu.domain.model.NomountStatus
import org.bakasu.bakasu.ui.component.ConfirmResult
import org.bakasu.bakasu.ui.component.folkMountWarningText
import org.bakasu.bakasu.ui.component.nomountDriverStateLabel
import org.bakasu.bakasu.ui.component.nomountUnavailableReason
import org.bakasu.bakasu.ui.component.rememberConfirmDialog
import org.bakasu.bakasu.ui.component.settings.SegmentedColumn
import org.bakasu.bakasu.ui.component.settings.SettingsChooseWidget
import org.bakasu.bakasu.ui.component.settings.SettingsJumpPageWidget
import org.bakasu.bakasu.ui.navigation.LocalNavigator
import org.bakasu.bakasu.ui.navigation.Route
import org.bakasu.bakasu.ui.util.LocalSnackbarHost
import org.bakasu.bakasu.ui.util.showReplacingSnackbar
import org.bakasu.bakasu.ui.viewmodel.FolkMountViewModel
import org.koin.compose.viewmodel.koinViewModel

private val MODE_ORDER = listOf(
    FolkMountMode.AUTO,
    FolkMountMode.BUILTIN,
    FolkMountMode.METAMODULE,
    FolkMountMode.NOMOUNT,
    FolkMountMode.OFF,
)

@Composable
private fun FolkMountMode.labelRes(): Int = when (this) {
    FolkMountMode.AUTO -> R.string.settings_folk_mount_mode_auto
    FolkMountMode.BUILTIN -> R.string.settings_folk_mount_mode_builtin
    FolkMountMode.METAMODULE -> R.string.settings_folk_mount_mode_metamodule
    FolkMountMode.NOMOUNT -> R.string.settings_folk_mount_mode_nomount
    FolkMountMode.OFF -> R.string.settings_folk_mount_mode_off
}

@Composable
private fun FolkMountMode.summaryRes(): Int = when (this) {
    FolkMountMode.AUTO -> R.string.settings_folk_mount_mode_auto_summary
    FolkMountMode.BUILTIN -> R.string.settings_folk_mount_mode_builtin_summary
    FolkMountMode.METAMODULE -> R.string.settings_folk_mount_mode_metamodule_summary
    FolkMountMode.NOMOUNT -> R.string.settings_folk_mount_mode_nomount_summary
    FolkMountMode.OFF -> R.string.settings_folk_mount_mode_off_summary
}

@Composable
private fun FolkMountProvider.labelRes(): Int = when (this) {
    FolkMountProvider.BUILTIN -> R.string.settings_folk_mount_provider_builtin
    FolkMountProvider.METAMODULE -> R.string.settings_folk_mount_provider_metamodule
    FolkMountProvider.NOMOUNT -> R.string.settings_folk_mount_provider_nomount
    FolkMountProvider.NONE -> R.string.settings_folk_mount_provider_none
}

@Composable
fun FolkMountTab(
    innerPadding: PaddingValues,
    nestedScrollConnection: NestedScrollConnection,
    modifier: Modifier = Modifier,
) {
    val viewModel: FolkMountViewModel = koinViewModel()
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val snackBarHost = LocalSnackbarHost.current
    val navigator = LocalNavigator.current
    val scope = rememberCoroutineScope()
    val confirm = rememberConfirmDialog()
    val nomountTitle = stringResource(R.string.settings_folk_mount_mode_nomount)
    val nomountConfirm = stringResource(R.string.settings_folk_mount_nomount_confirm)
    val nomountConfirmOk = stringResource(R.string.settings_folk_mount_nomount_confirm_ok)

    LaunchedEffect(viewModel) {
        viewModel.refresh()
    }
    LaunchedEffect(viewModel) {
        viewModel.events.collectLatest { message ->
            snackBarHost.showReplacingSnackbar(message)
        }
    }

    val status = uiState.status
    val available = status != null && !uiState.readFailed
    val modeIndex = status?.configuredMode?.let { MODE_ORDER.indexOf(it) }?.coerceAtLeast(0) ?: 0
    val modeItems = MODE_ORDER.map { stringResource(it.labelRes()) }
    val modeDescriptions = MODE_ORDER.map { stringResource(it.summaryRes()) }

    val summary = when {
        uiState.isLoading -> stringResource(R.string.settings_folk_mount_summary)
        status == null -> stringResource(R.string.settings_folk_mount_unavailable_summary)
        else -> folkMountSummary(status)
    }

    LazyColumn(
        modifier = modifier
            .fillMaxSize()
            .nestedScroll(nestedScrollConnection),
    ) {
        item {
            Spacer(Modifier.height(innerPadding.calculateTopPadding()))
        }
        item {
            SegmentedColumn(title = stringResource(R.string.settings_folk_mount)) {
                item {
                    SettingsChooseWidget(
                        icon = Icons.TwoTone.Layers,
                        title = stringResource(R.string.settings_folk_mount),
                        description = summary,
                        items = modeItems,
                        itemDescriptions = modeDescriptions,
                        enabled = available && !uiState.isLoading && !uiState.isWriting,
                        selectedIndex = modeIndex,
                        onSelectedIndexChange = { index ->
                            val picked = MODE_ORDER.getOrNull(index) ?: return@SettingsChooseWidget
                            if (status != null && !status.shouldWrite(picked)) return@SettingsChooseWidget
                            if (picked == FolkMountMode.NOMOUNT) {
                                scope.launch {
                                    val answer = confirm.awaitConfirm(
                                        title = nomountTitle,
                                        content = nomountConfirm,
                                        confirm = nomountConfirmOk,
                                    )
                                    if (answer is ConfirmResult.Confirmed) viewModel.setMode(picked)
                                }
                            } else {
                                viewModel.setMode(picked)
                            }
                        },
                    )
                }
                item {
                    SettingsJumpPageWidget(
                        icon = Icons.TwoTone.Layers,
                        title = stringResource(R.string.settings_nomount),
                        description = nomountSummary(
                            status = uiState.nomountStatus,
                            readError = uiState.nomountReadError,
                        ),
                        onClick = { navigator.push(Route.Nomount) },
                    )
                }
            }
        }
        item {
            Spacer(Modifier.height(innerPadding.calculateBottomPadding()))
        }
    }
}

@Composable
private fun folkMountSummary(status: FolkMountStatus): String {
    val base = stringResource(status.configuredMode.summaryRes())
    val bootLine = when (val provider = status.bootProvider) {
        null -> stringResource(R.string.settings_folk_mount_boot_provider_unknown)

        FolkMountProvider.NONE -> stringResource(R.string.settings_folk_mount_provider_none)

        else -> stringResource(
            R.string.settings_folk_mount_boot_provider,
            stringResource(provider.labelRes()),
        )
    }
    val compatHint = when {
        status.configuredMode == FolkMountMode.BUILTIN && status.metamoduleEnabled ->
            stringResource(R.string.settings_folk_mount_metamodule_compat_hint)

        status.configuredMode == FolkMountMode.NOMOUNT &&
            status.bootProvider != null &&
            status.bootProvider != FolkMountProvider.NOMOUNT ->
            folkMountWarningText(status) ?: stringResource(R.string.settings_folk_mount_nomount_unavailable_hint)

        else -> null
    }
    return listOfNotNull(
        base,
        bootLine,
        stringResource(R.string.settings_folk_mount_reboot_hint),
        compatHint,
    ).joinToString("\n")
}

@Composable
private fun nomountSummary(status: NomountStatus?, readError: Boolean): String = when {
    readError -> stringResource(R.string.nomount_read_error)

    status == null -> stringResource(R.string.nomount_loading)

    !status.apiOk -> nomountUnavailableReason(status)
        ?: stringResource(
            R.string.nomount_driver_unavailable,
            nomountDriverStateLabel(status.driverState),
        )

    else -> stringResource(R.string.nomount_rules_value, status.rulesApplied, status.rulesExpected)
}
