package org.bakasu.bakasu.ui.screen.main

import android.net.Uri
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.twotone.Article
import androidx.compose.material.icons.twotone.Adb
import androidx.compose.material.icons.twotone.BugReport
import androidx.compose.material.icons.twotone.ElectricalServices
import androidx.compose.material.icons.twotone.Extension
import androidx.compose.material.icons.twotone.FolderDelete
import androidx.compose.material.icons.twotone.FolderOff
import androidx.compose.material.icons.twotone.GppGood
import androidx.compose.material.icons.twotone.Policy
import androidx.compose.material.icons.twotone.RemoveCircle
import androidx.compose.material.icons.twotone.RemoveModerator
import androidx.compose.material.icons.twotone.RestartAlt
import androidx.compose.material.icons.twotone.Science
import androidx.compose.material.icons.twotone.Security
import androidx.compose.material.icons.twotone.Settings
import androidx.compose.material.icons.twotone.Update
import androidx.compose.material.icons.twotone.VerifiedUser
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.LargeFlexibleTopAppBar
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberTopAppBarState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import org.bakasu.bakasu.R
import org.bakasu.bakasu.domain.usecase.GenerateBugreportUseCase
import org.bakasu.bakasu.ui.component.SwipeableSnackbarHost
import org.bakasu.bakasu.ui.component.rememberLoadingDialog
import org.bakasu.bakasu.ui.component.settings.AppBackButton
import org.bakasu.bakasu.ui.component.settings.SegmentedColumn
import org.bakasu.bakasu.ui.component.settings.SegmentedColumnScope
import org.bakasu.bakasu.ui.component.settings.SettingsBaseWidget
import org.bakasu.bakasu.ui.component.settings.SettingsChooseWidget
import org.bakasu.bakasu.ui.component.settings.SettingsJumpPageWidget
import org.bakasu.bakasu.ui.component.settings.SettingsSwitchWidget
import org.bakasu.bakasu.ui.navigation.LocalNavigator
import org.bakasu.bakasu.ui.navigation.Route
import org.bakasu.bakasu.ui.theme.blurEffect
import org.bakasu.bakasu.ui.theme.blurSource
import org.bakasu.bakasu.ui.util.LocalSnackbarHost
import org.bakasu.bakasu.ui.util.showReplacingSnackbar
import org.bakasu.bakasu.ui.viewmodel.HomeViewModel
import org.bakasu.bakasu.ui.viewmodel.SettingsUiAction
import org.bakasu.bakasu.ui.viewmodel.SettingsViewModel
import org.koin.compose.koinInject
import org.koin.compose.viewmodel.koinViewModel

@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun SettingsCategoryScaffold(
    @StringRes titleRes: Int,
    content: SegmentedColumnScope.() -> Unit,
) {
    val navigator = LocalNavigator.current
    val settingsViewModel = koinViewModel<SettingsViewModel>()
    val scrollBehavior = TopAppBarDefaults.exitUntilCollapsedScrollBehavior(rememberTopAppBarState())
    val snackBarHost = LocalSnackbarHost.current

    LaunchedEffect(Unit) {
        settingsViewModel.dispatch(SettingsUiAction.LoadFeatureSettings)
    }

    Scaffold(
        topBar = {
            Column(modifier = Modifier.blurEffect()) {
                LargeFlexibleTopAppBar(
                    title = { Text(stringResource(titleRes)) },
                    scrollBehavior = scrollBehavior,
                    navigationIcon = {
                        AppBackButton(
                            onClick = { navigator.pop() },
                        )
                    },
                    colors = TopAppBarDefaults.topAppBarColors().copy(
                        containerColor = Color.Transparent,
                        scrolledContainerColor = Color.Transparent,
                    ),
                    windowInsets = TopAppBarDefaults.windowInsets.add(WindowInsets(left = 12.dp)),
                )
            }
        },
        snackbarHost = { SwipeableSnackbarHost(hostState = snackBarHost) },
        containerColor = Color.Transparent,
        modifier = Modifier.fillMaxSize(),
    ) { innerPadding ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .nestedScroll(scrollBehavior.nestedScrollConnection)
                .blurSource(),
            contentPadding = PaddingValues(
                top = innerPadding.calculateTopPadding() + 8.dp,
                start = 0.dp,
                end = 0.dp,
                bottom = innerPadding.calculateBottomPadding() + 8.dp,
            ),
        ) {
            item {
                SegmentedColumn(
                    modifier = Modifier.fillMaxWidth(),
                    content = content,
                )
            }
        }
    }
}

@Composable
fun SettingsCoreScreen() {
    val navigator = LocalNavigator.current
    val settingsViewModel = koinViewModel<SettingsViewModel>()
    val homeViewModel = koinViewModel<HomeViewModel>()
    val uiState by settingsViewModel.uiState.collectAsStateWithLifecycle()
    val homeState by homeViewModel.uiState.collectAsStateWithLifecycle()

    SettingsCategoryScaffold(R.string.configuration) {
        item {
            // 配置文件模板入口
            SettingsJumpPageWidget(
                icon = Icons.TwoTone.GppGood,
                title = stringResource(R.string.settings_profile_template),
                description = stringResource(R.string.settings_profile_template_summary),
                onClick = {
                    navigator.push(Route.AppProfileTemplate)
                },
            )
        }

        item {
            val modeItems = listOf(
                stringResource(id = R.string.settings_mode_default),
                stringResource(id = R.string.settings_mode_disable_until_reboot),
                stringResource(id = R.string.settings_mode_disable_always),
            )

            val suSummary = when (uiState.suStatus) {
                "unsupported" -> stringResource(id = R.string.feature_status_unsupported_summary)
                "managed" -> stringResource(id = R.string.feature_status_managed_summary)
                "forced" -> stringResource(id = R.string.feature_status_forced_summary)
                else -> stringResource(id = R.string.settings_sucompat_summary)
            }
            SettingsChooseWidget(
                icon = Icons.TwoTone.RemoveModerator,
                title = stringResource(id = R.string.settings_sucompat),
                description = suSummary,
                items = modeItems,
                enabled = uiState.suStatus == "supported",
                selectedIndex = uiState.suCompatMode,
                onSelectedIndexChange = { index ->
                    settingsViewModel.dispatch(SettingsUiAction.SetSuCompatMode(index))
                },
            )
        }

        item {
            val umountSummary = when (uiState.kernelUmountStatus) {
                "unsupported" -> stringResource(id = R.string.feature_status_unsupported_summary)
                "managed" -> stringResource(id = R.string.feature_status_managed_summary)
                "forced" -> stringResource(id = R.string.feature_status_forced_summary)
                else -> stringResource(id = R.string.settings_kernel_umount_summary)
            }
            SettingsSwitchWidget(
                icon = Icons.TwoTone.RemoveCircle,
                title = stringResource(id = R.string.settings_kernel_umount),
                description = umountSummary,
                enabled = uiState.kernelUmountStatus == "supported",
                checked = uiState.isKernelUmountEnabled,
                onCheckedChange = { enabled ->
                    settingsViewModel.dispatch(SettingsUiAction.SetKernelUmount(enabled))
                },
            )
        }

        item(visible = homeState.systemStatus.isLateLoadMode) {
            SettingsSwitchWidget(
                icon = Icons.TwoTone.ElectricalServices,
                title = stringResource(id = R.string.settings_auto_jailbreak),
                description = stringResource(id = R.string.settings_auto_jailbreak_summary),
                checked = uiState.autoJailbreakEnabled,
                onCheckedChange = { value ->
                    settingsViewModel.dispatch(SettingsUiAction.SetAutoJailbreak(value))
                },
            )
        }

        item(visible = Build.VERSION.SDK_INT > Build.VERSION_CODES.Q) {
            val adbRootSummary = when (uiState.adbRootStatus) {
                "unsupported" -> stringResource(id = R.string.feature_status_unsupported_summary)
                "managed" -> stringResource(id = R.string.feature_status_managed_summary)
                "forced" -> stringResource(id = R.string.feature_status_forced_summary)
                else -> stringResource(id = R.string.settings_adb_root_summary)
            }
            SettingsSwitchWidget(
                icon = Icons.TwoTone.Adb,
                title = stringResource(id = R.string.settings_adb_root),
                description = adbRootSummary,
                checked = uiState.isAdbRootEnabled,
                enabled = uiState.adbRootStatus == "supported",
                onCheckedChange = { enabled ->
                    settingsViewModel.dispatch(SettingsUiAction.SetAdbRoot(enabled))
                },
            )
        }

        item {
            val sulogSummary = when (uiState.sulogStatus) {
                "unsupported" -> stringResource(id = R.string.feature_status_unsupported_summary)
                "managed" -> stringResource(id = R.string.feature_status_managed_summary)
                "forced" -> stringResource(id = R.string.feature_status_forced_summary)
                else -> stringResource(id = R.string.settings_sulog_summary)
            }
            SettingsSwitchWidget(
                icon = Icons.AutoMirrored.TwoTone.Article,
                title = stringResource(id = R.string.settings_sulog),
                description = sulogSummary,
                enabled = uiState.sulogStatus == "supported",
                checked = uiState.isSuLogEnabled,
                onCheckedChange = { enabled ->
                    settingsViewModel.dispatch(SettingsUiAction.SetSuLog(enabled))
                },
            )
        }

        item {
            val selinuxHideSummary = when (uiState.selinuxHideStatus) {
                "unsupported" -> stringResource(id = R.string.feature_status_unsupported_summary)
                "managed" -> stringResource(id = R.string.feature_status_managed_summary)
                "forced" -> stringResource(id = R.string.feature_status_forced_summary)
                else -> stringResource(id = R.string.settings_selinux_hide_summary)
            }
            SettingsSwitchWidget(
                icon = Icons.TwoTone.Policy,
                title = stringResource(id = R.string.settings_selinux_hide),
                description = selinuxHideSummary,
                enabled = uiState.selinuxHideStatus == "supported",
                checked = uiState.isSelinuxHideEnabled,
                onCheckedChange = { checked ->
                    settingsViewModel.dispatch(SettingsUiAction.SetSelinuxHide(checked))
                },
            )
        }

        item {
            val shellSuAdbOnlySummary = when (uiState.shellSuAdbOnlyStatus) {
                "unsupported" -> stringResource(id = R.string.feature_status_unsupported_summary)
                "managed" -> stringResource(id = R.string.feature_status_managed_summary)
                "forced" -> stringResource(id = R.string.feature_status_forced_summary)
                else -> stringResource(id = R.string.settings_shell_su_adb_only_summary)
            }
            SettingsSwitchWidget(
                icon = Icons.TwoTone.VerifiedUser,
                title = stringResource(id = R.string.settings_shell_su_adb_only),
                description = shellSuAdbOnlySummary,
                enabled = uiState.shellSuAdbOnlyStatus == "supported",
                checked = uiState.isShellSuAdbOnlyEnabled,
                onCheckedChange = { enabled ->
                    settingsViewModel.dispatch(SettingsUiAction.SetShellSuAdbOnly(enabled))
                },
            )
        }

        item {
            // 卸载模块开关
            SettingsSwitchWidget(
                icon = Icons.TwoTone.FolderDelete,
                title = stringResource(id = R.string.settings_umount_modules_default),
                description = stringResource(id = R.string.settings_umount_modules_default_summary),
                checked = uiState.defaultUmountModules,
                onCheckedChange = { enabled ->
                    settingsViewModel.dispatch(SettingsUiAction.SetDefaultUmountModules(enabled))
                },
            )
        }

        item {
            // 软重启开关 (重启时优先软重启, 保留 jailbreak)
            SettingsSwitchWidget(
                icon = Icons.TwoTone.RestartAlt,
                title = stringResource(id = R.string.settings_soft_reboot),
                description = stringResource(id = R.string.settings_soft_reboot_summary),
                enabled = homeState.systemStatus.isFullFeatured && !homeState.systemStatus.isLateLoadMode,
                checked = homeState.systemStatus.isLateLoadMode || uiState.useSoftReboot,
                onCheckedChange = { enabled ->
                    settingsViewModel.dispatch(SettingsUiAction.SetUseSoftReboot(enabled))
                },
            )
        }
    }
}

@Composable
fun SettingsAppScreen() {
    val settingsViewModel = koinViewModel<SettingsViewModel>()
    val uiState by settingsViewModel.uiState.collectAsStateWithLifecycle()
    val navigator = LocalNavigator.current

    SettingsCategoryScaffold(R.string.app_settings) {
        expandableItem(
            expanded = uiState.checkManagerUpdate,
            topContent = {
                SettingsSwitchWidget(
                    icon = Icons.TwoTone.Update,
                    title = stringResource(R.string.settings_check_manager_update),
                    description = stringResource(R.string.settings_check_manager_update_summary),
                    checked = uiState.checkManagerUpdate,
                    onCheckedChange = { enabled ->
                        settingsViewModel.dispatch(SettingsUiAction.SetManagerUpdateCheck(enabled))
                    },
                )
            },
        ) {
            item(topPadding = 1.dp) {
                SettingsSwitchWidget(
                    icon = Icons.TwoTone.Science,
                    title = stringResource(R.string.settings_check_beta_update),
                    description = stringResource(R.string.settings_check_beta_update_summary),
                    checked = uiState.checkBetaUpdate,
                    onCheckedChange = { enabled ->
                        settingsViewModel.dispatch(SettingsUiAction.SetBetaUpdateCheck(enabled))
                    },
                )
            }
        }

        item {
            SettingsSwitchWidget(
                icon = Icons.TwoTone.Extension,
                title = stringResource(R.string.settings_check_module_update),
                description = stringResource(R.string.settings_check_module_update_summary),
                checked = uiState.checkModuleUpdate,
                onCheckedChange = { enabled ->
                    settingsViewModel.dispatch(SettingsUiAction.SetModuleUpdateCheck(enabled))
                },
            )
        }

        item {
            // 更多设置
            SettingsJumpPageWidget(
                icon = Icons.TwoTone.Settings,
                title = stringResource(R.string.theme_settings),
                description = stringResource(R.string.theme_settings),
                onClick = {
                    navigator.push(Route.ThemeSettings)
                },
            )
        }
    }
}

@Composable
fun SettingsToolsScreen() {
    val navigator = LocalNavigator.current
    val settingsViewModel = koinViewModel<SettingsViewModel>()
    val homeViewModel = koinViewModel<HomeViewModel>()
    val uiState by settingsViewModel.uiState.collectAsStateWithLifecycle()
    val homeState by homeViewModel.uiState.collectAsStateWithLifecycle()

    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackBarHost = LocalSnackbarHost.current
    val loadingDialog = rememberLoadingDialog()
    val generateBugreport = koinInject<GenerateBugreportUseCase>()
    val logSaved = stringResource(R.string.log_saved)
    val exportBugreportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/gzip"),
    ) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch(Dispatchers.IO) {
            loadingDialog.show()
            context.contentResolver.openOutputStream(uri)?.use { output ->
                generateBugreport().inputStream().use { it.copyTo(output) }
            }
            loadingDialog.hide()
            snackBarHost.showReplacingSnackbar(logSaved)
        }
    }

    SettingsCategoryScaffold(R.string.tools) {
        item {
            SettingsBaseWidget(
                icon = Icons.TwoTone.BugReport,
                title = stringResource(R.string.send_log),
                onClick = {
                    exportBugreportLauncher.launch("bugreport.tar.gz")
                },
            ) {}
        }

        item(visible = homeState.systemStatus.isFullFeatured) {
            SettingsJumpPageWidget(
                icon = Icons.TwoTone.Security,
                title = stringResource(R.string.dynamic_manager_title),
                description = stringResource(R.string.dynamic_manager_settings_summary),
                onClick = {
                    navigator.push(Route.DynamicManager)
                },
            )
        }

        item(visible = homeState.systemStatus.isFullFeatured && uiState.isKernelUmountEnabled) {
            SettingsJumpPageWidget(
                icon = Icons.TwoTone.FolderOff,
                title = stringResource(R.string.umount_path_manager),
                description = stringResource(R.string.umount_path_manager_summary),
                onClick = {
                    navigator.push(Route.UmountManager)
                },
            )
        }

        item(visible = homeState.systemStatus.lkmMode == true && !homeState.systemStatus.isLateLoadMode) {
            UninstallItem {
                loadingDialog.withLoading(it)
            }
        }
    }
}
