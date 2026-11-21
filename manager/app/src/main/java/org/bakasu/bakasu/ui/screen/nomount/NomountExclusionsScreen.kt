package org.bakasu.bakasu.ui.screen.nomount

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Deselect
import androidx.compose.material.icons.filled.SelectAll
import androidx.compose.material.icons.filled.SwapVert
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LargeFlexibleTopAppBar
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberTopAppBarState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch
import org.bakasu.bakasu.R
import org.bakasu.bakasu.ui.component.PackageIcon
import org.bakasu.bakasu.ui.component.material.ExpressiveScaffold
import org.bakasu.bakasu.ui.component.material.SegmentedColumn
import org.bakasu.bakasu.ui.component.material.SegmentedListItem
import org.bakasu.bakasu.ui.component.material.SegmentedSwitchItem
import org.bakasu.bakasu.ui.component.material.SegmentedTextField
import org.bakasu.bakasu.ui.component.material.TopBarBackButton
import org.bakasu.bakasu.ui.component.material.expressiveTopAppBarColors
import org.bakasu.bakasu.ui.navigation.LocalNavigator
import org.bakasu.bakasu.ui.theme.CardConfig
import org.bakasu.bakasu.ui.theme.ThemeConfig
import org.bakasu.bakasu.ui.theme.blurEffect
import org.bakasu.bakasu.ui.theme.blurSource
import org.bakasu.bakasu.ui.viewmodel.NomountViewModel
import org.koin.compose.koinInject
import org.koin.compose.viewmodel.koinViewModel

@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun NomountExclusionsScreen() {
    val viewModel = koinViewModel<NomountViewModel>()
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    val navigator = LocalNavigator.current
    val themeConfig: ThemeConfig = koinInject()
    val cardConfig: CardConfig = koinInject()
    val scrollBehavior = TopAppBarDefaults.exitUntilCollapsedScrollBehavior(rememberTopAppBarState())

    LaunchedEffect(Unit) { viewModel.loadApps() }

    var selected by remember(state.status?.uids) {
        mutableStateOf(state.status?.uids?.toSet() ?: emptySet())
    }
    var query by rememberSaveable { mutableStateOf("") }
    val scope = rememberCoroutineScope()

    val visible = state.apps.filter { app ->
        val matchesSystem = state.showSystemApps || !app.isSystem
        val matchesQuery = query.isBlank() ||
            app.label.contains(query, ignoreCase = true) ||
            app.packageName.contains(query, ignoreCase = true)
        matchesSystem && matchesQuery
    }

    val chromeColor = if (themeConfig.isEnableBlur) {
        Color.Transparent
    } else {
        MaterialTheme.colorScheme.surfaceContainer.copy(cardConfig.cardAlpha)
    }

    ExpressiveScaffold(
        containerColor = Color.Transparent,
        topBar = {
            Column(modifier = Modifier.blurEffect()) {
                LargeFlexibleTopAppBar(
                    title = { Text(stringResource(R.string.nomount_exclusions_title)) },
                    navigationIcon = { TopBarBackButton(onClick = { navigator.pop() }) },
                    actions = {
                        TextButton(
                            enabled = !state.isWriting,
                            onClick = {
                                viewModel.saveExclusions(selected.toList())
                                navigator.pop()
                            },
                        ) {
                            Text(stringResource(R.string.nomount_save))
                        }
                    },
                    colors = expressiveTopAppBarColors(
                        containerColor = chromeColor,
                        scrolledContainerColor = chromeColor,
                    ),
                    scrollBehavior = scrollBehavior,
                )
            }
        },
    ) { inner ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .blurSource()
                .padding(inner)
                .nestedScroll(scrollBehavior.nestedScrollConnection),
        ) {
            SegmentedColumn(modifier = Modifier.padding(horizontal = 16.dp)) {
                item {
                    SegmentedTextField(
                        label = stringResource(R.string.nomount_search_apps),
                        value = query,
                        onValueChange = { query = it },
                        singleLine = true,
                    )
                }
                item {
                    SegmentedSwitchItem(
                        title = stringResource(R.string.nomount_show_system_apps),
                        checked = state.showSystemApps,
                        onCheckedChange = viewModel::setShowSystemApps,
                    )
                }
                item {
                    SegmentedListItem(
                        headlineContent = {
                            Text(stringResource(R.string.nomount_follow_profiles))
                        },
                        supportingContent = {
                            Text(stringResource(R.string.nomount_follow_profiles_summary))
                        },
                        enabled = !state.appsLoading && !state.isWriting,
                        onClick = {
                            scope.launch {
                                selected = selected +
                                    viewModel.profileExcludedUids(state.showSystemApps)
                            }
                        },
                    )
                }
            }

            val visibleUids = visible.map { it.uid }
            val canEdit = !state.appsLoading && !state.isWriting
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = 24.dp, end = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = stringResource(R.string.nomount_selected_count, selected.size),
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.weight(1f),
                )
                IconButton(
                    enabled = canEdit && visibleUids.isNotEmpty(),
                    onClick = { selected = selected + visibleUids },
                ) {
                    Icon(
                        Icons.Filled.SelectAll,
                        contentDescription = stringResource(R.string.nomount_select_all),
                    )
                }
                IconButton(
                    enabled = canEdit && visibleUids.isNotEmpty(),
                    onClick = {
                        selected = visibleUids.fold(selected) { acc, uid ->
                            if (acc.contains(uid)) acc - uid else acc + uid
                        }
                    },
                ) {
                    Icon(
                        Icons.Filled.SwapVert,
                        contentDescription = stringResource(R.string.nomount_invert_selection),
                    )
                }
                IconButton(
                    enabled = canEdit && selected.isNotEmpty(),
                    onClick = { selected = selected - visibleUids.toSet() },
                ) {
                    Icon(
                        Icons.Filled.Deselect,
                        contentDescription = stringResource(R.string.nomount_clear_selection),
                    )
                }
            }

            if (state.appsLoading) {
                Text(
                    text = stringResource(R.string.nomount_loading),
                    modifier = Modifier.padding(16.dp),
                )
            } else {
                LazyColumn(
                    modifier = Modifier.weight(1f),
                    contentPadding = PaddingValues(16.dp),
                    verticalArrangement = Arrangement.spacedBy(2.dp),
                ) {
                    items(visible, key = { it.uid }) { app ->
                        val checked = selected.contains(app.uid)
                        SegmentedListItem(
                            modifier = Modifier.fillMaxWidth(),
                            enabled = !state.isWriting,
                            onClick = {
                                selected = if (checked) {
                                    selected + app.uid
                                } else {
                                    selected - app.uid
                                }
                            },
                            leadingContent = {
                                PackageIcon(
                                    packageName = app.packageName,
                                    contentDescription = app.label,
                                    modifier = Modifier.size(40.dp),
                                )
                            },
                            headlineContent = { Text(app.label) },
                            supportingContent = {
                                Text(
                                    stringResource(
                                        R.string.nomount_app_uid,
                                        app.packageName,
                                        app.uid,
                                    ),
                                )
                            },
                            trailingContent = {
                                Checkbox(
                                    checked = checked,
                                    onCheckedChange = { nowChecked ->
                                        selected = if (nowChecked) {
                                            selected + app.uid
                                        } else {
                                            selected - app.uid
                                        }
                                    },
                                )
                            },
                        )
                    }
                }
            }
        }
    }
}
