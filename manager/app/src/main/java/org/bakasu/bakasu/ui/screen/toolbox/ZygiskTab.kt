package org.bakasu.bakasu.ui.screen.toolbox

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.twotone.Android
import androidx.compose.material.icons.twotone.Block
import androidx.compose.material.icons.twotone.BugReport
import androidx.compose.material.icons.twotone.Extension
import androidx.compose.material.icons.twotone.Info
import androidx.compose.material.icons.twotone.Memory
import androidx.compose.material.icons.twotone.Refresh
import androidx.compose.material.icons.twotone.RestartAlt
import androidx.compose.material.icons.twotone.Security
import androidx.compose.material.icons.twotone.Settings
import androidx.compose.material.icons.twotone.Speed
import androidx.compose.material.icons.twotone.VisibilityOff
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import org.bakasu.bakasu.Natives
import org.bakasu.bakasu.R
import org.bakasu.bakasu.domain.model.ZygiskRuntimeRecord
import org.bakasu.bakasu.domain.model.ZygiskState
import org.bakasu.bakasu.getKernelVersion
import org.bakasu.bakasu.ui.component.SwipeableSnackbarHost
import org.bakasu.bakasu.ui.component.WarningCard
import org.bakasu.bakasu.ui.component.settings.SegmentedColumn
import org.bakasu.bakasu.ui.component.settings.SettingsBaseWidget
import org.bakasu.bakasu.ui.component.settings.SettingsChooseWidget
import org.bakasu.bakasu.ui.component.settings.SettingsSwitchWidget
import org.bakasu.bakasu.ui.component.settings.lazySegmentColumn
import org.bakasu.bakasu.ui.screen.LabelText
import org.bakasu.bakasu.ui.util.showReplacingSnackbar
import org.bakasu.bakasu.ui.viewmodel.ZygiskUiEvent
import org.bakasu.bakasu.ui.viewmodel.ZygiskViewModel
import org.koin.compose.viewmodel.koinViewModel

/** 注入记录过多时只渲染前若干条, 避免一次构建上千行。 */
private const val MAX_VISIBLE_RECORDS = 200

/**
 * 工具箱 → YukiZygisk 管理器。
 *
 * 功能面对齐 Rouyashiki/YukiZygisk 的 yzctl: 概览、开关、运行时配置、注入记录。
 */
@Composable
fun ZygiskTab(
    innerPadding: PaddingValues,
    nestedScrollConnection: NestedScrollConnection,
) {
    val viewModel: ZygiskViewModel = koinViewModel()
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val snackBarHostState = remember { SnackbarHostState() }

    // 仿 SUSFS / IO 调度器: 骨架先显示, 过渡动画结束后再加载
    var pageLoaded by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        pageLoaded = false
        delay(400) // 等 folkx 过渡动画结束
        viewModel.refresh()
        pageLoaded = true
    }
    LaunchedEffect(viewModel) {
        viewModel.events.collectLatest { event ->
            when (event) {
                is ZygiskUiEvent.Message -> snackBarHostState.showReplacingSnackbar(event.message)
            }
        }
    }

    val supported = pageLoaded && uiState.supported
    val records = remember(uiState.records) { uiState.records.take(MAX_VISIBLE_RECORDS) }

    // lazySegmentColumn 的 title 是普通参数, 不能在其内部调用 @Composable, 提到这里取好
    val runtimeTitle = stringResource(R.string.zygisk_runtime_title)

    Box(modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .nestedScroll(nestedScrollConnection),
        ) {
            item {
                Spacer(Modifier.height(innerPadding.calculateTopPadding()))
            }

            item {
                ZygiskOverviewSection(
                    uiState = uiState,
                    pageLoaded = pageLoaded,
                    onToggleEnabled = { viewModel.toggleEnabled(it) },
                )
            }

            item {
                Spacer(Modifier.height(20.dp))
            }

            item {
                ZygiskOptionsSection(
                    uiState = uiState,
                    enabled = supported,
                    onToggleYukilinker = { viewModel.setSwitchOption("yukilinker", it) },
                    onToggleAnonymous = { viewModel.setSwitchOption("anonymous_memory", it) },
                    onToggleEarlyLoad = { viewModel.setSwitchOption("early_load", it) },
                    onToggleDmesg = { viewModel.setSwitchOption("dmesg_log", it) },
                    onToggleCrashProtection = { viewModel.setSwitchOption("crash_protection", it) },
                    onSelectDenylistMode = { viewModel.setDenylistMode(it) },
                    onReload = { viewModel.reload() },
                    onRefreshSnapshot = { viewModel.refreshEarlySnapshot() },
                    onEnsureDaemon = { viewModel.startDaemon() },
                )
            }

            if (supported) {
                item {
                    Spacer(Modifier.height(20.dp))
                }

                if (records.isEmpty()) {
                    item {
                        ZygiskEmptyHint(stringResource(R.string.zygisk_runtime_empty))
                    }
                } else {
                    lazySegmentColumn(
                        items = records,
                        title = runtimeTitle,
                        key = { index, record -> record.pid.toString() + "#" + record.generation + "#" + index },
                        contentType = { _, _ -> "zygisk-runtime" },
                    ) { _, record ->
                        ZygiskRuntimeItem(record = record)
                    }
                }

                if (uiState.records.size > MAX_VISIBLE_RECORDS) {
                    item {
                        ZygiskEmptyHint(
                            stringResource(
                                R.string.zygisk_runtime_truncated,
                                MAX_VISIBLE_RECORDS,
                                uiState.records.size,
                            ),
                        )
                    }
                }
            }

            item {
                Spacer(Modifier.height(20.dp))
            }

            item {
                SegmentedColumn {
                    item {
                        SettingsBaseWidget(
                            icon = Icons.TwoTone.Info,
                            title = stringResource(R.string.zygisk_about_title),
                            description = stringResource(R.string.zygisk_about_summary),
                        )
                    }
                }
            }

            item {
                Spacer(Modifier.height(24.dp))
            }
        }

        SwipeableSnackbarHost(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(horizontal = 16.dp, vertical = 12.dp),
            hostState = snackBarHostState,
        )
    }
}

@Composable
private fun ZygiskEmptyHint(text: String) {
    Box(
        modifier = Modifier.fillMaxWidth().padding(vertical = 16.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(text = text, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun ZygiskOverviewSection(
    uiState: ZygiskState,
    pageLoaded: Boolean,
    onToggleEnabled: (Boolean) -> Unit,
) {
    val lkmMode = remember { Natives.isLkmMode }
    val modeLabel = stringResource(if (lkmMode) R.string.zygisk_mode_lkm else R.string.zygisk_mode_builtin)
    val supported = pageLoaded && uiState.supported
    // YukiZygisk 只在内核 5.10 - 6.18 之间参与编译 (kernel/Kbuild 的 YUKIZYGISK_KVER 门控),
    // 两端越界时给出明确提示, 而不是笼统地报「内核不支持」。
    val kernelVersion = remember { getKernelVersion() }
    val kernelTooNew = kernelVersion.major > 6 || (kernelVersion.major == 6 && kernelVersion.patchLevel >= 19)
    val kernelTooOld = kernelVersion.major in 1..4 || (kernelVersion.major == 5 && kernelVersion.patchLevel < 10)
    val unsupportedText = stringResource(
        when {
            kernelTooNew -> R.string.zygisk_status_unsupported_kernel
            kernelTooOld -> R.string.zygisk_status_unsupported_old_kernel
            else -> R.string.zygisk_status_unsupported
        },
    )
    val statusText = when {
        !pageLoaded -> stringResource(R.string.zygisk_status_loading)
        !uiState.supported -> unsupportedText
        uiState.enabled -> stringResource(R.string.zygisk_status_active)
        else -> stringResource(R.string.zygisk_status_inactive)
    }
    SegmentedColumn {
        item {
            SettingsBaseWidget(
                icon = Icons.TwoTone.Android,
                title = stringResource(R.string.zygisk_title),
                description = statusText,
                descriptionColumnContent = {
                    if (supported) {
                        Row(
                            modifier = Modifier.padding(top = 4.dp),
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            LabelText(label = modeLabel)
                            if (uiState.safeMode) {
                                LabelText(
                                    label = stringResource(R.string.zygisk_safe_mode_badge),
                                    containerColor = MaterialTheme.colorScheme.errorContainer,
                                    contentColor = MaterialTheme.colorScheme.onErrorContainer,
                                )
                            }
                            if (uiState.capabilities != 0) {
                                LabelText(
                                    label = stringResource(R.string.zygisk_capabilities_badge, uiState.capabilities),
                                )
                            }
                        }
                    }
                },
                trailingContent = {
                    Switch(
                        checked = uiState.enabled,
                        enabled = supported && !uiState.isSubmitting,
                        onCheckedChange = onToggleEnabled,
                    )
                },
            )
        }
        item(visible = supported) {
            SettingsBaseWidget(
                icon = Icons.TwoTone.Speed,
                title = stringResource(R.string.zygisk_status_generation),
                description = stringResource(R.string.zygisk_status_generation_desc, uiState.generation),
            )
        }
        item(visible = supported) {
            SettingsBaseWidget(
                icon = Icons.TwoTone.Extension,
                title = stringResource(R.string.zygisk_status_targets),
                description = stringResource(
                    R.string.zygisk_status_targets_desc,
                    uiState.activeCount,
                    uiState.records.size,
                ),
            )
        }
        item(visible = supported) {
            SettingsBaseWidget(
                icon = Icons.TwoTone.BugReport,
                title = stringResource(R.string.zygisk_status_zygote_crashes),
                description = stringResource(R.string.zygisk_status_zygote_crashes_desc, uiState.zygoteCrashes),
            )
        }
        item(visible = supported && uiState.safeModeZygote.isNotBlank()) {
            SettingsBaseWidget(
                icon = Icons.TwoTone.Block,
                title = stringResource(R.string.zygisk_status_safe_mode_zygote),
                description = uiState.safeModeZygote,
                isError = true,
            )
        }
        item(visible = pageLoaded && uiState.error != null) {
            WarningCard(
                modifier = Modifier.padding(top = 8.dp),
                message = uiState.error.orEmpty(),
                color = MaterialTheme.colorScheme.errorContainer,
            )
        }
    }
}

@Composable
private fun ZygiskOptionsSection(
    uiState: ZygiskState,
    enabled: Boolean,
    onToggleYukilinker: (Boolean) -> Unit,
    onToggleAnonymous: (Boolean) -> Unit,
    onToggleEarlyLoad: (Boolean) -> Unit,
    onToggleDmesg: (Boolean) -> Unit,
    onToggleCrashProtection: (Boolean) -> Unit,
    onSelectDenylistMode: (Int) -> Unit,
    onReload: () -> Unit,
    onRefreshSnapshot: () -> Unit,
    onEnsureDaemon: () -> Unit,
) {
    val interactive = enabled && !uiState.isSubmitting
    val denylistItems = listOf(
        stringResource(R.string.zygisk_denylist_mode_inject),
        stringResource(R.string.zygisk_denylist_mode_skip),
        stringResource(R.string.zygisk_denylist_mode_inject_umount),
    )
    val denylistDescriptions = listOf(
        stringResource(R.string.zygisk_denylist_mode_inject_desc),
        stringResource(R.string.zygisk_denylist_mode_skip_desc),
        stringResource(R.string.zygisk_denylist_mode_inject_umount_desc),
    )
    SegmentedColumn(title = stringResource(R.string.zygisk_options_title)) {
        item {
            SettingsSwitchWidget(
                icon = Icons.TwoTone.Memory,
                title = stringResource(R.string.zygisk_yukilinker_title),
                description = stringResource(R.string.zygisk_yukilinker_summary),
                checked = uiState.config.yukilinker,
                enabled = interactive,
                onCheckedChange = onToggleYukilinker,
            )
        }
        item {
            SettingsSwitchWidget(
                icon = Icons.TwoTone.VisibilityOff,
                title = stringResource(R.string.zygisk_anonymous_title),
                description = stringResource(R.string.zygisk_anonymous_summary),
                checked = uiState.config.anonymousMemory,
                enabled = interactive,
                onCheckedChange = onToggleAnonymous,
            )
        }
        item {
            SettingsSwitchWidget(
                icon = Icons.TwoTone.RestartAlt,
                title = stringResource(R.string.zygisk_early_load_title),
                description = stringResource(R.string.zygisk_early_load_summary),
                checked = uiState.config.earlyLoad,
                enabled = interactive,
                onCheckedChange = onToggleEarlyLoad,
            )
        }
        item {
            SettingsChooseWidget(
                icon = Icons.TwoTone.Block,
                iconPlaceholder = false,
                title = stringResource(R.string.zygisk_denylist_title),
                description = stringResource(R.string.zygisk_denylist_summary),
                enabled = interactive,
                items = denylistItems,
                itemDescriptions = denylistDescriptions,
                selectedIndex = uiState.config.denylistMode.coerceIn(0, 2),
                onSelectedIndexChange = onSelectDenylistMode,
            )
        }
        item {
            SettingsSwitchWidget(
                icon = Icons.TwoTone.Settings,
                title = stringResource(R.string.zygisk_dmesg_title),
                description = stringResource(R.string.zygisk_dmesg_summary),
                checked = uiState.config.dmesgLog,
                enabled = interactive,
                onCheckedChange = onToggleDmesg,
            )
        }
        item {
            SettingsSwitchWidget(
                icon = Icons.TwoTone.Security,
                title = stringResource(R.string.zygisk_crash_protection_title),
                description = stringResource(R.string.zygisk_crash_protection_summary),
                checked = uiState.config.crashProtection,
                enabled = interactive,
                onCheckedChange = onToggleCrashProtection,
            )
        }
        item {
            SettingsBaseWidget(
                icon = Icons.TwoTone.Refresh,
                title = stringResource(R.string.zygisk_reload_title),
                description = stringResource(R.string.zygisk_reload_summary),
                enabled = interactive,
                onClick = { onReload() },
            )
        }
        item {
            SettingsBaseWidget(
                icon = Icons.TwoTone.RestartAlt,
                title = stringResource(R.string.zygisk_refresh_snapshot_title),
                description = stringResource(R.string.zygisk_refresh_snapshot_summary),
                enabled = interactive,
                onClick = { onRefreshSnapshot() },
            )
        }
        item {
            SettingsBaseWidget(
                icon = Icons.TwoTone.Settings,
                title = stringResource(R.string.zygisk_ensure_daemon_title),
                description = stringResource(R.string.zygisk_ensure_daemon_summary),
                enabled = interactive,
                onClick = { onEnsureDaemon() },
            )
        }
    }
}

@Composable
private fun ZygiskRuntimeItem(record: ZygiskRuntimeRecord) {
    val title = record.process.ifBlank { record.target }
    SettingsBaseWidget(
        icon = Icons.TwoTone.Extension,
        title = title,
        description = stringResource(R.string.zygisk_runtime_summary, record.pid, record.state, record.injectionState),
        isError = record.isFailed,
        descriptionColumnContent = {
            Row(
                modifier = Modifier.padding(top = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                LabelText(label = record.abi)
                LabelText(label = record.kind)
                if (record.generation > 0) {
                    LabelText(label = stringResource(R.string.zygisk_runtime_generation_badge, record.generation))
                }
                if (record.restarts > 0) {
                    LabelText(
                        label = stringResource(R.string.zygisk_runtime_restarts_badge, record.restarts),
                        containerColor = MaterialTheme.colorScheme.errorContainer,
                        contentColor = MaterialTheme.colorScheme.onErrorContainer,
                    )
                }
            }
            if (record.target.isNotBlank() && record.target != title) {
                Text(
                    text = record.target,
                    modifier = Modifier.padding(top = 2.dp),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            if (record.module.isNotBlank()) {
                Text(
                    text = stringResource(R.string.zygisk_runtime_module, record.module),
                    modifier = Modifier.padding(top = 2.dp),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        },
    )
}
