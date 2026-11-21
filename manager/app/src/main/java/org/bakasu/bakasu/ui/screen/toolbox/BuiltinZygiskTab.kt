package org.bakasu.bakasu.ui.screen.toolbox

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.twotone.Build
import androidx.compose.material.icons.twotone.CheckCircle
import androidx.compose.material.icons.twotone.Error
import androidx.compose.material.icons.twotone.Extension
import androidx.compose.material.icons.twotone.Security
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LoadingIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch
import org.bakasu.bakasu.R
import org.bakasu.bakasu.domain.model.BuiltinZygiskConfig
import org.bakasu.bakasu.domain.model.InjectedZygiskModule
import org.bakasu.bakasu.domain.model.InstalledModule
import org.bakasu.bakasu.ui.component.settings.SegmentedColumn
import org.bakasu.bakasu.ui.component.settings.SettingsBaseWidget
import org.bakasu.bakasu.ui.component.settings.SettingsSwitchWidget
import org.bakasu.bakasu.ui.util.LocalSnackbarHost
import org.bakasu.bakasu.ui.util.showReplacingSnackbar
import org.bakasu.bakasu.ui.viewmodel.BuiltinZygiskUiEvent
import org.bakasu.bakasu.ui.viewmodel.BuiltinZygiskUiState
import org.bakasu.bakasu.ui.viewmodel.BuiltinZygiskViewModel
import org.koin.compose.viewmodel.koinViewModel

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun BuiltinZygiskTab(
    innerPadding: PaddingValues,
    nestedScrollConnection: NestedScrollConnection,
    modifier: Modifier = Modifier,
) {
    val viewModel = koinViewModel<BuiltinZygiskViewModel>()
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val snackbarHost = LocalSnackbarHost.current
    val scope = rememberCoroutineScope()
    var pendingSwitch by remember { mutableStateOf<String?>(null) }
    val actionsEnabled = !uiState.isBusy
    val pageLoaded = rememberToolboxPageLoaded { viewModel.refresh() }

    LaunchedEffect(viewModel) {
        viewModel.events.collect { event ->
            when (event) {
                is BuiltinZygiskUiEvent.Message -> snackbarHost.showReplacingSnackbar(event.message)
                is BuiltinZygiskUiEvent.Switched -> pendingSwitch = event.targetName
            }
        }
    }

    LazyColumn(
        modifier = modifier
            .fillMaxSize()
            .nestedScroll(nestedScrollConnection),
        contentPadding = PaddingValues(
            top = innerPadding.calculateTopPadding(),
            bottom = innerPadding.calculateBottomPadding(),
        ),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (!pageLoaded || uiState.isLoading) {
            item {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 48.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    LoadingIndicator()
                }
            }
        } else {
            item {
                StatusSection(
                    uiState = uiState,
                    actionsEnabled = actionsEnabled,
                    onRefresh = viewModel::refresh,
                    onRearm = viewModel::rearm,
                    onSetEnabled = viewModel::setEnabled,
                )
            }
            item {
                OptionsSection(
                    config = uiState.config,
                    enabled = actionsEnabled && uiState.status.enabled,
                    onSetOption = viewModel::setOption,
                )
            }
            item {
                ModulesSection(
                    modules = uiState.providerModules,
                    enabled = actionsEnabled,
                    onSwitch = viewModel::switchTo,
                )
            }
        }
    }

    pendingSwitch?.let { name ->
        AlertDialog(
            onDismissRequest = { pendingSwitch = null },
            title = { Text(stringResource(R.string.builtin_zygisk_switch_reboot_title)) },
            text = { Text(stringResource(R.string.builtin_zygisk_switch_reboot_message, name)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        pendingSwitch = null
                        // The handover keeps running after the panel closes; restarting
                        // first would interrupt ksud halfway through it.
                        scope.launch {
                            if (viewModel.awaitPendingSwitch()) {
                                viewModel.softReboot()
                            }
                        }
                    },
                ) {
                    Text(stringResource(R.string.builtin_zygisk_soft_restart))
                }
            },
            dismissButton = {
                TextButton(onClick = { pendingSwitch = null }) {
                    Text(stringResource(android.R.string.cancel))
                }
            },
        )
    }
}

@Composable
private fun StatusSection(
    uiState: BuiltinZygiskUiState,
    actionsEnabled: Boolean,
    onRefresh: () -> Unit,
    onRearm: () -> Unit,
    onSetEnabled: (Boolean) -> Unit,
) {
    val status = uiState.status
    val runningText = stringResource(R.string.builtin_zygisk_status_running)
    val stoppedText = stringResource(R.string.builtin_zygisk_status_stopped)
    val injectedText = stringResource(R.string.builtin_zygisk_state_injected)
    val notInjectedText = stringResource(R.string.builtin_zygisk_zygote_not_injected)
    val versionText = if (status.version.isBlank()) {
        stringResource(R.string.builtin_zygisk_impl_hint)
    } else {
        stringResource(R.string.builtin_zygisk_version, status.version)
    }
    val implementationName = uiState.implementation.name.ifBlank { stringResource(R.string.builtin_zygisk_provider) }

    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SegmentedColumn(title = stringResource(R.string.builtin_zygisk_status_title)) {
            item {
                SettingsBaseWidget(
                    icon = Icons.TwoTone.Extension,
                    title = implementationName,
                    description = versionText,
                    onClick = { onRearm() },
                    enabled = actionsEnabled && status.enabled,
                    trailingContent = {
                        IconButton(onClick = onRefresh, enabled = actionsEnabled) {
                            Icon(
                                imageVector = Icons.Filled.Refresh,
                                contentDescription = stringResource(R.string.builtin_zygisk_refresh),
                            )
                        }
                    },
                )
            }
            item {
                SettingsSwitchWidget(
                    icon = Icons.TwoTone.Extension,
                    title = stringResource(R.string.builtin_zygisk_enable),
                    description = stringResource(R.string.builtin_zygisk_enable_summary),
                    checked = status.enabled,
                    enabled = actionsEnabled,
                    onCheckedChange = onSetEnabled,
                )
            }
            item {
                SettingsBaseWidget(
                    icon = Icons.TwoTone.Security,
                    title = stringResource(R.string.builtin_zygisk_monitor_status),
                    description = if (status.monitorRunning) runningText else stoppedText,
                    trailingContent = { StatusIcon(status.monitorRunning) },
                )
            }
            item {
                SettingsBaseWidget(
                    icon = Icons.TwoTone.Build,
                    title = stringResource(R.string.builtin_zygisk_daemon_status),
                    description = if (status.daemonRunning) runningText else stoppedText,
                    trailingContent = { StatusIcon(status.daemonRunning) },
                )
            }
            for (zygote in status.zygotes) {
                item(key = zygote.abi) {
                    val injected = zygote.status == InjectedZygiskModule.STATE_INJECTED
                    SettingsBaseWidget(
                        icon = Icons.TwoTone.Extension,
                        iconPlaceholder = false,
                        title = stringResource(R.string.builtin_zygisk_zygote_status, zygote.abi),
                        description = if (injected) injectedText else notInjectedText,
                        trailingContent = { StatusIcon(injected) },
                    )
                }
            }
        }

        SegmentedColumn(
            title = stringResource(R.string.builtin_zygisk_injected_group, uiState.injectedModules.size),
        ) {
            item(visible = uiState.injectedModules.isEmpty()) {
                SettingsBaseWidget(
                    icon = Icons.TwoTone.Extension,
                    title = stringResource(R.string.builtin_zygisk_injected_empty),
                    enabled = false,
                )
            }
            for (module in uiState.injectedModules) {
                item(key = module.id) {
                    InjectedModuleRow(module)
                }
            }
        }
    }
}

@Composable
private fun InjectedModuleRow(module: InjectedZygiskModule) {
    val stateText = stringResource(injectedStateText(module.state))
    val summary = listOf(module.version, stateText).filter { it.isNotBlank() }.joinToString(" · ")
    SettingsBaseWidget(
        icon = Icons.TwoTone.Extension,
        iconPlaceholder = false,
        title = module.name,
        description = summary.takeIf { it.isNotBlank() },
        descriptionColumnContent = {
            if (module.targets.isNotEmpty()) {
                Text(
                    text = stringResource(R.string.builtin_zygisk_targets, module.targets.joinToString(", ")),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            if (module.processes.isEmpty()) {
                Text(
                    text = stringResource(R.string.builtin_zygisk_no_processes),
                    style = MaterialTheme.typography.bodySmall,
                )
            } else {
                for (process in module.processes) {
                    Text(
                        text = stringResource(R.string.builtin_zygisk_process_entry, process.name, process.pid),
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        },
    )
}

@Composable
private fun OptionsSection(
    config: BuiltinZygiskConfig,
    enabled: Boolean,
    onSetOption: (String, String) -> Unit,
) {
    SegmentedColumn(title = stringResource(R.string.builtin_zygisk_options_title)) {
        item {
            SettingsSwitchWidget(
                title = stringResource(R.string.builtin_zygisk_opt_anonymous_memory),
                description = stringResource(R.string.builtin_zygisk_opt_anonymous_memory_summary),
                checked = config.anonymousMemory,
                enabled = enabled,
                onCheckedChange = {
                    onSetOption(BuiltinZygiskViewModel.OPTION_ANONYMOUS_MEMORY, if (it) "1" else "0")
                },
            )
        }
        item {
            SettingsSwitchWidget(
                title = stringResource(R.string.builtin_zygisk_opt_custom_linker),
                description = stringResource(R.string.builtin_zygisk_opt_custom_linker_summary),
                checked = config.customLinker,
                enabled = enabled,
                onCheckedChange = {
                    onSetOption(BuiltinZygiskViewModel.OPTION_CUSTOM_LINKER, if (it) "1" else "0")
                },
            )
        }
        item {
            SettingsSwitchWidget(
                title = stringResource(R.string.builtin_zygisk_opt_yukiplt),
                description = stringResource(R.string.builtin_zygisk_opt_yukiplt_summary),
                checked = config.pltEngine != BuiltinZygiskConfig.PLT_ENGINE_LSPLT,
                enabled = enabled,
                onCheckedChange = {
                    onSetOption(
                        BuiltinZygiskViewModel.OPTION_PLT_ENGINE,
                        if (it) BuiltinZygiskConfig.PLT_ENGINE_YUKIPLT else BuiltinZygiskConfig.PLT_ENGINE_LSPLT,
                    )
                },
            )
        }
        item {
            SettingsBaseWidget(
                title = stringResource(R.string.builtin_zygisk_options_hint),
                description = null,
                enabled = false,
            )
        }
    }
}

@Composable
private fun ModulesSection(
    modules: List<InstalledModule>,
    enabled: Boolean,
    onSwitch: (String, String) -> Unit,
) {
    SegmentedColumn(title = stringResource(R.string.builtin_zygisk_modules_title)) {
        item(visible = modules.isEmpty()) {
            SettingsBaseWidget(
                icon = Icons.TwoTone.Extension,
                title = stringResource(R.string.builtin_zygisk_no_modules),
                enabled = false,
            )
        }
        for (module in modules) {
            item(key = module.id) {
                SettingsBaseWidget(
                    modifier = Modifier.fillMaxWidth(),
                    icon = Icons.TwoTone.Extension,
                    title = module.name,
                    description = module.version,
                    descriptionColumnContent = {
                        if (module.suppressedByBuiltin) {
                            Text(
                                text = stringResource(R.string.builtin_zygisk_suppressed_hint),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.error,
                            )
                        }
                    },
                    enabled = enabled,
                    onClick = { onSwitch(module.id, module.name) },
                )
            }
        }
    }
}

@Composable
private fun StatusIcon(ok: Boolean) {
    Icon(
        imageVector = if (ok) Icons.TwoTone.CheckCircle else Icons.TwoTone.Error,
        contentDescription = null,
        tint = if (ok) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
    )
}

private fun injectedStateText(state: String): Int = when (state) {
    InjectedZygiskModule.STATE_INJECTED -> R.string.builtin_zygisk_state_injected
    InjectedZygiskModule.STATE_LOADED -> R.string.builtin_zygisk_state_loaded
    InjectedZygiskModule.STATE_FAILED -> R.string.builtin_zygisk_state_failed
    else -> R.string.builtin_zygisk_state_unknown
}
