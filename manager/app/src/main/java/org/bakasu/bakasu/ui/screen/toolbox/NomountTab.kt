package org.bakasu.bakasu.ui.screen.toolbox

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.rememberTextFieldState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.twotone.Add
import androidx.compose.material.icons.twotone.Block
import androidx.compose.material.icons.twotone.Delete
import androidx.compose.material.icons.twotone.Extension
import androidx.compose.material.icons.twotone.Folder
import androidx.compose.material.icons.twotone.Info
import androidx.compose.material.icons.twotone.Memory
import androidx.compose.material.icons.twotone.Refresh
import androidx.compose.material.icons.twotone.Security
import androidx.compose.material.icons.twotone.Settings
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Switch
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
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import org.bakasu.bakasu.Natives
import org.bakasu.bakasu.R
import org.bakasu.bakasu.data.packageinfo.InstalledPackageRepository
import org.bakasu.bakasu.data.packageinfo.PER_USER_RANGE
import org.bakasu.bakasu.domain.model.NomountModule
import org.bakasu.bakasu.domain.model.NomountRule
import org.bakasu.bakasu.domain.model.NomountState
import org.bakasu.bakasu.getKernelVersion
import org.bakasu.bakasu.ui.component.ConfirmResult
import org.bakasu.bakasu.ui.component.PackageIcon
import org.bakasu.bakasu.ui.component.SwipeableSnackbarHost
import org.bakasu.bakasu.ui.component.WarningCard
import org.bakasu.bakasu.ui.component.rememberConfirmDialog
import org.bakasu.bakasu.ui.component.rememberCustomDialog
import org.bakasu.bakasu.ui.component.settings.SegmentedColumn
import org.bakasu.bakasu.ui.component.settings.SettingsBaseWidget
import org.bakasu.bakasu.ui.component.settings.SettingsSwitchWidget
import org.bakasu.bakasu.ui.component.settings.SettingsTextFieldWidget
import org.bakasu.bakasu.ui.component.settings.lazySegmentColumn
import org.bakasu.bakasu.ui.screen.LabelText
import org.bakasu.bakasu.ui.util.showReplacingSnackbar
import org.bakasu.bakasu.ui.viewmodel.NomountUiEvent
import org.bakasu.bakasu.ui.viewmodel.NomountViewModel
import org.koin.compose.koinInject
import org.koin.compose.viewmodel.koinViewModel

/** 规则数量过多时只渲染前若干条, 避免一次构建上万行。 */
private const val MAX_VISIBLE_RULES = 200

/** 规则分组里的条目: 首项是「添加规则」入口, 其余是内核中的现有规则。 */
private sealed interface RuleRow {
    data object Add : RuleRow

    data class Entry(val rule: NomountRule) : RuleRow
}

/** UID 排除分组里的条目: 首项是「添加排除」入口, 其余是已排除的 UID。 */
private sealed interface ExclusionRow {
    data object Add : ExclusionRow

    data class Entry(val uid: Int) : ExclusionRow
}

/**
 * 工具箱 → NoMount 管理器。
 *
 * 功能面对齐 maxsteeel/nomount 的 WebUI: 概览、选项、模块热挂载、规则与 UID 排除。
 */
@Composable
fun NomountTab(
    innerPadding: PaddingValues,
    nestedScrollConnection: NestedScrollConnection,
) {
    val viewModel: NomountViewModel = koinViewModel()
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val snackBarHostState = remember { SnackbarHostState() }
    val confirmDialog = rememberConfirmDialog()
    val scope = rememberCoroutineScope()
    val packageRepository: InstalledPackageRepository = koinInject()

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
                is NomountUiEvent.Message -> snackBarHostState.showReplacingSnackbar(event.message)
            }
        }
    }

    val ruleDialog = rememberCustomDialog { dismiss ->
        NomountRuleDialog(
            onDismiss = dismiss,
            onConfirm = { virtualPath, realPath, whiteout, opaque ->
                dismiss()
                viewModel.addRule(virtualPath, realPath, whiteout, opaque)
            },
        )
    }
    val exclusionDialog = rememberCustomDialog { dismiss ->
        NomountExclusionDialog(
            packageRepository = packageRepository,
            onDismiss = dismiss,
            onConfirm = { uid ->
                dismiss()
                viewModel.addExclusion(uid)
            },
        )
    }

    val supported = pageLoaded && uiState.supported
    val ruleRows: List<RuleRow> = remember(uiState.rules) {
        buildList {
            add(RuleRow.Add)
            uiState.rules.take(MAX_VISIBLE_RULES).forEach { add(RuleRow.Entry(it)) }
        }
    }
    val exclusionRows: List<ExclusionRow> = remember(uiState.exclusions) {
        buildList {
            add(ExclusionRow.Add)
            uiState.exclusions.forEach { add(ExclusionRow.Entry(it)) }
        }
    }
    val packages by packageRepository.packages.collectAsStateWithLifecycle()
    val packageByUid = remember(packages) { packages.associateBy { it.uid } }

    // lazySegmentColumn 的 title 是普通参数, 不能在其内部调用 @Composable, 提到这里取好
    val modulesTitle = stringResource(R.string.nomount_modules_title)
    val rulesTitle = stringResource(R.string.nomount_rules_title)
    val exclusionsTitle = stringResource(R.string.nomount_exclusions_title)

    // 对话框文案同上, 同样要在 composable 内取好 —— context.getString 在 Compose 里会被
    // LocalContextGetResourceValueCall 判定为配置不安全。
    val clearRulesTitle = stringResource(R.string.nomount_clear_rules_confirm_title)
    val clearRulesMessage = stringResource(R.string.nomount_clear_rules_confirm_message)
    val deleteRuleTitle = stringResource(R.string.nomount_delete_rule_title)

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
                NomountOverviewSection(
                    uiState = uiState,
                    pageLoaded = pageLoaded,
                    supported = supported,
                    onToggleEnabled = { viewModel.setEnabled(it) },
                )
            }

            item {
                Spacer(Modifier.height(20.dp))
            }

            item {
                NomountOptionsSection(
                    uiState = uiState,
                    enabled = supported,
                    onToggleSafeMode = { viewModel.setSafeMode(it) },
                    onToggleIsolated = { viewModel.setBlockIsolated(it) },
                    onInject = { viewModel.inject() },
                    onClearRules = {
                        scope.launch {
                            val result = confirmDialog.awaitConfirm(
                                title = clearRulesTitle,
                                content = clearRulesMessage,
                            )
                            if (result == ConfirmResult.Confirmed) {
                                viewModel.clearRules()
                            }
                        }
                    },
                )
            }

            if (supported) {
                item {
                    Spacer(Modifier.height(20.dp))
                }

                if (uiState.modules.isEmpty()) {
                    item {
                        NomountEmptyHint(stringResource(R.string.nomount_modules_empty))
                    }
                } else {
                    lazySegmentColumn(
                        items = uiState.modules,
                        title = modulesTitle,
                        key = { _, module -> module.id },
                        contentType = { _, _ -> "nomount-module" },
                    ) { _, module ->
                        NomountModuleItem(
                            module = module,
                            busy = uiState.isSubmitting,
                            onLoad = { viewModel.loadModule(module.id) },
                            onUnload = { viewModel.unloadModule(module.id) },
                            onToggleSkipMount = {
                                viewModel.setModuleSkipMount(module.id, !module.skipMount)
                            },
                            onToggleDisabled = { viewModel.setModuleDisabled(module.id, it) },
                        )
                    }
                }

                item {
                    Spacer(Modifier.height(20.dp))
                }

                lazySegmentColumn(
                    items = ruleRows,
                    title = rulesTitle,
                    key = { index, row ->
                        when (row) {
                            RuleRow.Add -> "nomount-rule-add"
                            is RuleRow.Entry -> "nomount-rule-" + row.rule.virtualPath + "@" + row.rule.uid
                        }
                    },
                    contentType = { _, row ->
                        when (row) {
                            RuleRow.Add -> "nomount-rule-add"
                            is RuleRow.Entry -> "nomount-rule"
                        }
                    },
                ) { _, row ->
                    when (row) {
                        RuleRow.Add -> SettingsBaseWidget(
                            icon = Icons.TwoTone.Add,
                            title = stringResource(R.string.nomount_rule_add_title),
                            description = stringResource(R.string.nomount_rule_add_summary),
                            onClick = { ruleDialog.show() },
                        )

                        is RuleRow.Entry -> NomountRuleItem(
                            rule = row.rule,
                            onDelete = {
                                scope.launch {
                                    val result = confirmDialog.awaitConfirm(
                                        title = deleteRuleTitle,
                                        content = row.rule.virtualPath,
                                    )
                                    if (result == ConfirmResult.Confirmed) {
                                        viewModel.removeRule(row.rule.virtualPath)
                                    }
                                }
                            },
                        )
                    }
                }

                if (uiState.rules.size > MAX_VISIBLE_RULES) {
                    item {
                        NomountEmptyHint(
                            stringResource(
                                R.string.nomount_rules_truncated,
                                MAX_VISIBLE_RULES,
                                uiState.rules.size,
                            ),
                        )
                    }
                }

                item {
                    Spacer(Modifier.height(20.dp))
                }

                if (uiState.exclusions.isEmpty()) {
                    item {
                        NomountEmptyHint(stringResource(R.string.nomount_exclusions_empty))
                    }
                }

                lazySegmentColumn(
                    items = exclusionRows,
                    title = exclusionsTitle,
                    key = { _, row ->
                        when (row) {
                            ExclusionRow.Add -> "nomount-exclusion-add"
                            is ExclusionRow.Entry -> "nomount-exclusion-" + row.uid
                        }
                    },
                    contentType = { _, row ->
                        when (row) {
                            ExclusionRow.Add -> "nomount-exclusion-add"
                            is ExclusionRow.Entry -> "nomount-exclusion"
                        }
                    },
                ) { _, row ->
                    when (row) {
                        ExclusionRow.Add -> SettingsBaseWidget(
                            icon = Icons.TwoTone.Add,
                            title = stringResource(R.string.nomount_exclusion_add),
                            description = stringResource(R.string.nomount_exclusion_add_summary),
                            onClick = { exclusionDialog.show() },
                        )

                        is ExclusionRow.Entry -> NomountExclusionItem(
                            uid = row.uid,
                            packageName = packageByUid[row.uid]?.packageName,
                            appLabel = packageByUid[row.uid]?.appLabel,
                            onRemove = { viewModel.removeExclusion(row.uid) },
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
                            title = stringResource(R.string.nomount_about_title),
                            description = stringResource(R.string.nomount_about_summary),
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

/** 条目下方的小徽章行 (规则标记 / UID 标记)。 */
@Composable
private fun NomountBadgeRow(
    spacing: Dp = 4.dp,
    content: @Composable RowScope.() -> Unit,
) {
    Row(
        modifier = Modifier.padding(top = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(spacing),
        verticalAlignment = Alignment.CenterVertically,
        content = content,
    )
}

/** 列表条目右侧的删除按钮。 */
@Composable
private fun NomountDeleteButton(onClick: () -> Unit) {
    IconButton(onClick = onClick) {
        Icon(
            imageVector = Icons.TwoTone.Delete,
            contentDescription = stringResource(R.string.delete),
            tint = MaterialTheme.colorScheme.error,
        )
    }
}

@Composable
private fun NomountEmptyHint(text: String) {
    Box(
        modifier = Modifier.fillMaxWidth().padding(vertical = 16.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(text = text, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/** 概览区里的只读信息行 (驱动 / 规则数 / 模块数)。 */
@Composable
private fun NomountInfoItem(
    icon: ImageVector,
    title: String,
    description: String,
) {
    SettingsBaseWidget(
        icon = icon,
        title = title,
        description = description,
    )
}

@Composable
private fun NomountOverviewSection(
    uiState: NomountState,
    pageLoaded: Boolean,
    supported: Boolean,
    onToggleEnabled: (Boolean) -> Unit,
) {
    val lkmMode = remember { Natives.isLkmMode }
    val modeLabel = stringResource(if (lkmMode) R.string.nomount_mode_lkm else R.string.nomount_mode_builtin)
    // NoMount 只在内核 4.9 - 6.17 之间参与编译 (上游仅在 GKI 5.10 - 6.12 上验证),
    // 两端越界时给出明确提示, 而不是笼统地报「内核不支持」。
    val kernelVersion = remember { getKernelVersion() }
    val kernelTooNew = kernelVersion.major > 6 || (kernelVersion.major == 6 && kernelVersion.patchLevel >= 18)
    val kernelTooOld = kernelVersion.major in 1..3 || (kernelVersion.major == 4 && kernelVersion.patchLevel < 9)
    val unsupportedText = stringResource(
        when {
            kernelTooNew -> R.string.nomount_status_unsupported_kernel
            kernelTooOld -> R.string.nomount_status_unsupported_old_kernel
            else -> R.string.nomount_status_unsupported
        },
    )
    val statusText = when {
        !pageLoaded -> stringResource(R.string.nomount_status_loading)
        !uiState.supported -> unsupportedText
        uiState.enabled -> stringResource(R.string.nomount_status_active)
        else -> stringResource(R.string.nomount_status_inactive)
    }
    SegmentedColumn {
        item {
            SettingsBaseWidget(
                icon = Icons.TwoTone.Memory,
                title = stringResource(R.string.nomount_title),
                description = statusText,
                descriptionColumnContent = {
                    if (supported) {
                        NomountBadgeRow(spacing = 6.dp) {
                            LabelText(label = modeLabel)
                            if (uiState.safeMode) {
                                LabelText(
                                    label = stringResource(R.string.nomount_safe_mode_badge),
                                    containerColor = MaterialTheme.colorScheme.errorContainer,
                                    contentColor = MaterialTheme.colorScheme.onErrorContainer,
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
            NomountInfoItem(
                icon = Icons.TwoTone.Extension,
                title = stringResource(R.string.nomount_status_driver),
                description = stringResource(R.string.nomount_status_driver_desc, uiState.version),
            )
        }
        item(visible = supported) {
            NomountInfoItem(
                icon = Icons.TwoTone.Folder,
                title = stringResource(R.string.nomount_status_rules),
                description = stringResource(R.string.nomount_status_rules_desc, uiState.rules.size),
            )
        }
        item(visible = supported) {
            NomountInfoItem(
                icon = Icons.TwoTone.Settings,
                title = stringResource(R.string.nomount_status_modules),
                description = stringResource(
                    R.string.nomount_status_modules_desc,
                    uiState.activeModuleCount,
                    uiState.modules.size,
                ),
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
private fun NomountOptionsSection(
    uiState: NomountState,
    enabled: Boolean,
    onToggleSafeMode: (Boolean) -> Unit,
    onToggleIsolated: (Boolean) -> Unit,
    onInject: () -> Unit,
    onClearRules: () -> Unit,
) {
    SegmentedColumn(title = stringResource(R.string.nomount_options_title)) {
        item {
            SettingsSwitchWidget(
                icon = Icons.TwoTone.Security,
                title = stringResource(R.string.nomount_safe_mode_title),
                description = stringResource(R.string.nomount_safe_mode_summary),
                checked = uiState.safeMode,
                enabled = enabled && !uiState.isSubmitting,
                onCheckedChange = onToggleSafeMode,
            )
        }
        item {
            SettingsSwitchWidget(
                icon = Icons.TwoTone.Block,
                title = stringResource(R.string.nomount_isolated_title),
                description = stringResource(R.string.nomount_isolated_summary),
                checked = uiState.isolatedBlocked,
                enabled = enabled && !uiState.isSubmitting,
                onCheckedChange = onToggleIsolated,
            )
        }
        item {
            SettingsBaseWidget(
                icon = Icons.TwoTone.Refresh,
                title = stringResource(R.string.nomount_inject_title),
                description = stringResource(R.string.nomount_inject_summary),
                enabled = enabled && !uiState.isSubmitting,
                onClick = { onInject() },
            )
        }
        item {
            SettingsBaseWidget(
                icon = Icons.TwoTone.Delete,
                iconColor = MaterialTheme.colorScheme.error,
                title = stringResource(R.string.nomount_clear_rules_title),
                description = stringResource(R.string.nomount_clear_rules_summary),
                enabled = enabled && !uiState.isSubmitting,
                onClick = { onClearRules() },
            )
        }
    }
}

@Composable
private fun NomountModuleItem(
    module: NomountModule,
    busy: Boolean,
    onLoad: () -> Unit,
    onUnload: () -> Unit,
    onToggleSkipMount: () -> Unit,
    onToggleDisabled: (Boolean) -> Unit,
) {
    SettingsBaseWidget(
        icon = Icons.TwoTone.Extension,
        title = module.name.ifBlank { module.id },
        description = stringResource(
            R.string.nomount_module_summary,
            module.fileCount,
            module.loaded,
        ),
        descriptionColumnContent = {
            Row(
                modifier = Modifier.padding(top = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                TextButton(
                    enabled = !busy,
                    onClick = if (module.isLoaded) onUnload else onLoad,
                ) {
                    Text(
                        text = stringResource(
                            if (module.isLoaded) {
                                R.string.nomount_module_unload
                            } else {
                                R.string.nomount_module_load
                            },
                        ),
                    )
                }
                TextButton(enabled = !busy, onClick = onToggleSkipMount) {
                    Text(
                        text = if (module.skipMount) {
                            stringResource(R.string.nomount_module_unskip)
                        } else {
                            stringResource(R.string.nomount_module_skip)
                        },
                    )
                }
            }
        },
        trailingContent = {
            Switch(
                checked = !module.disabled,
                enabled = !busy,
                onCheckedChange = onToggleDisabled,
            )
        },
    )
}

@Composable
private fun NomountRuleItem(
    rule: NomountRule,
    onDelete: () -> Unit,
) {
    val badges = buildList {
        if (rule.whiteout) add(stringResource(R.string.nomount_rule_whiteout))
        if (rule.opaque) add(stringResource(R.string.nomount_rule_opaque))
        if (rule.virtualDir) add(stringResource(R.string.nomount_rule_virtual_dir))
        if (rule.hasUidScope) {
            add(
                if (rule.uid == rule.uidEnd) {
                    stringResource(R.string.nomount_rule_uid, rule.uid)
                } else {
                    stringResource(R.string.nomount_rule_uid_range, rule.uid, rule.uidEnd)
                },
            )
        }
    }
    SettingsBaseWidget(
        icon = if (rule.whiteout) Icons.TwoTone.Block else Icons.TwoTone.Folder,
        title = rule.virtualPath,
        description = rule.realPath.ifBlank { null },
        descriptionColumnContent = {
            if (badges.isNotEmpty()) {
                NomountBadgeRow {
                    badges.forEach { LabelText(label = it) }
                }
            }
        },
        trailingContent = { NomountDeleteButton(onClick = onDelete) },
    )
}

@Composable
private fun NomountExclusionItem(
    uid: Int,
    packageName: String?,
    appLabel: String?,
    onRemove: () -> Unit,
) {
    val title = appLabel?.takeIf { it.isNotBlank() } ?: packageName ?: stringResource(
        R.string.nomount_exclusion_uid,
        uid,
    )
    val leading: (@Composable () -> Unit)? = if (packageName != null) {
        {
            PackageIcon(
                packageName = packageName,
                contentDescription = null,
                modifier = Modifier.size(40.dp),
            )
        }
    } else {
        null
    }
    SettingsBaseWidget(
        leadingContent = leading,
        iconPlaceholder = false,
        icon = if (packageName == null) Icons.TwoTone.Block else null,
        title = title,
        description = when {
            packageName == null -> stringResource(R.string.nomount_exclusion_uid_desc, uid)

            // 应用名缺失时 title 已经回落到包名, 再写一遍就是重复文案
            title == packageName -> null

            else -> packageName
        },
        descriptionColumnContent = {
            NomountBadgeRow {
                LabelText(label = stringResource(R.string.nomount_exclusion_uid, uid))
                val userIndex = uid / PER_USER_RANGE
                if (userIndex != 0) {
                    LabelText(
                        label = stringResource(R.string.nomount_exclusion_user, userIndex),
                        containerColor = MaterialTheme.colorScheme.secondaryContainer,
                        contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
                    )
                }
            }
        },
        trailingContent = { NomountDeleteButton(onClick = onRemove) },
    )
}

@Composable
private fun NomountDialogConfirmButton(
    enabled: Boolean,
    onClick: () -> Unit,
) {
    TextButton(enabled = enabled, onClick = onClick) {
        Text(stringResource(android.R.string.ok))
    }
}

@Composable
private fun NomountDialogDismissButton(onClick: () -> Unit) {
    TextButton(onClick = onClick) {
        Text(stringResource(android.R.string.cancel))
    }
}

@Composable
private fun NomountRuleDialog(
    onDismiss: () -> Unit,
    onConfirm: (virtualPath: String, realPath: String?, whiteout: Boolean, opaque: Boolean) -> Unit,
) {
    val virtualState = rememberTextFieldState()
    val realState = rememberTextFieldState()
    var whiteout by remember { mutableStateOf(false) }
    var opaque by remember { mutableStateOf(false) }

    val virtualPath = virtualState.text.toString().trim()
    val realPath = realState.text.toString().trim()
    val canConfirm = virtualPath.isNotEmpty() && (whiteout || realPath.isNotEmpty())

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.nomount_rule_add_title)) },
        text = {
            Column(modifier = Modifier.verticalScroll(rememberScrollState())) {
                SegmentedColumn(contentPadding = PaddingValues(0.dp)) {
                    item {
                        SettingsTextFieldWidget(
                            modifier = Modifier.fillMaxWidth(),
                            state = virtualState,
                            renderBackgroundBlur = false,
                            title = stringResource(R.string.nomount_rule_virtual),
                            lineLimits = TextFieldLineLimits.SingleLine,
                        )
                    }
                    item {
                        SettingsTextFieldWidget(
                            modifier = Modifier.fillMaxWidth(),
                            state = realState,
                            renderBackgroundBlur = false,
                            enabled = !whiteout,
                            title = stringResource(R.string.nomount_rule_real),
                            lineLimits = TextFieldLineLimits.SingleLine,
                        )
                    }
                    item {
                        SettingsSwitchWidget(
                            title = stringResource(R.string.nomount_rule_whiteout),
                            description = stringResource(R.string.nomount_rule_whiteout_summary),
                            checked = whiteout,
                            onCheckedChange = { checked ->
                                whiteout = checked
                                if (checked) opaque = false
                            },
                        )
                    }
                    item {
                        SettingsSwitchWidget(
                            title = stringResource(R.string.nomount_rule_opaque),
                            description = stringResource(R.string.nomount_rule_opaque_summary),
                            checked = opaque,
                            enabled = !whiteout,
                            onCheckedChange = { checked ->
                                opaque = checked
                                if (checked) whiteout = false
                            },
                        )
                    }
                }
            }
        },
        confirmButton = {
            NomountDialogConfirmButton(
                enabled = canConfirm,
                onClick = {
                    onConfirm(
                        virtualPath,
                        realPath.ifEmpty { null },
                        whiteout,
                        opaque,
                    )
                },
            )
        },
        dismissButton = { NomountDialogDismissButton(onClick = onDismiss) },
    )
}

@Composable
private fun NomountExclusionDialog(
    packageRepository: InstalledPackageRepository,
    onDismiss: () -> Unit,
    onConfirm: (uid: Int) -> Unit,
) {
    val packages by packageRepository.packages.collectAsStateWithLifecycle()
    val uidState = rememberTextFieldState()
    val manualUid = uidState.text.toString().trim().toIntOrNull()

    LaunchedEffect(Unit) {
        if (packages.isEmpty()) {
            packageRepository.refresh()
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.nomount_pick_app)) },
        text = {
            Column {
                SegmentedColumn(contentPadding = PaddingValues(0.dp)) {
                    item {
                        SettingsTextFieldWidget(
                            modifier = Modifier.fillMaxWidth(),
                            state = uidState,
                            renderBackgroundBlur = false,
                            title = stringResource(R.string.nomount_uid_label),
                            lineLimits = TextFieldLineLimits.SingleLine,
                        )
                    }
                }
                if (packages.isEmpty()) {
                    Box(
                        modifier = Modifier.fillMaxWidth().padding(vertical = 24.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            text = stringResource(R.string.nomount_pick_app_empty),
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                } else {
                    LazyColumn(modifier = Modifier.heightIn(max = 360.dp)) {
                        lazySegmentColumn(
                            items = packages,
                            key = { _, info -> info.packageName },
                            contentType = { _, _ -> "nomount-app" },
                        ) { _, info ->
                            SettingsBaseWidget(
                                leadingContent = {
                                    PackageIcon(
                                        packageName = info.packageName,
                                        contentDescription = null,
                                        modifier = Modifier.size(36.dp),
                                    )
                                },
                                iconPlaceholder = false,
                                title = info.appLabel.ifBlank { info.packageName },
                                description = info.packageName,
                                onClick = { onConfirm(info.uid) },
                                trailingContent = {
                                    Text(
                                        text = stringResource(R.string.nomount_pick_app_uid, info.uid),
                                        style = MaterialTheme.typography.labelSmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                        maxLines = 1,
                                        overflow = TextOverflow.Ellipsis,
                                    )
                                },
                            )
                        }
                    }
                }
            }
        },
        confirmButton = {
            NomountDialogConfirmButton(
                enabled = manualUid != null,
                onClick = { manualUid?.let(onConfirm) },
            )
        },
        dismissButton = { NomountDialogDismissButton(onClick = onDismiss) },
    )
}
