package org.bakasu.bakasu.ui.screen.toolbox

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.twotone.Check
import androidx.compose.material.icons.twotone.LockClock
import androidx.compose.material.icons.twotone.Speed
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.bakasu.bakasu.R
import org.bakasu.bakasu.ui.component.settings.SegmentedColumn
import org.bakasu.bakasu.ui.component.settings.SettingsBaseWidget
import org.bakasu.bakasu.ui.viewmodel.IoSchedulerViewModel
import org.koin.compose.viewmodel.koinViewModel

@Composable
fun IoSchedulerTab(
    innerPadding: PaddingValues,
    nestedScrollConnection: NestedScrollConnection,
) {
    val viewModel: IoSchedulerViewModel = koinViewModel()
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()

    val pageLoaded = rememberToolboxPageLoaded { viewModel.refresh() }

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .nestedScroll(nestedScrollConnection),
    ) {
        item {
            Spacer(Modifier.height(innerPadding.calculateTopPadding()))
        }

        item {
            SegmentedColumn {
                item {
                    val pinned = uiState.pinned
                    val pinDesc = if (pageLoaded && uiState.loaded && pinned != null) {
                        stringResource(R.string.iosched_pinned_desc, pinned)
                    } else {
                        stringResource(R.string.iosched_unpinned_desc)
                    }
                    SettingsBaseWidget(
                        icon = Icons.TwoTone.LockClock,
                        title = stringResource(R.string.iosched_pin),
                        description = pinDesc,
                        onClick = { viewModel.setPinned(uiState.pinned == null) },
                        trailingContent = {
                            Switch(
                                checked = uiState.pinned != null,
                                onCheckedChange = { viewModel.setPinned(it) },
                            )
                        },
                    )
                }
                item {
                    SettingsBaseWidget(
                        icon = Icons.TwoTone.Speed,
                        title = stringResource(R.string.iosched_current),
                        description = if (pageLoaded && uiState.loaded) {
                            if (uiState.current.isNotBlank()) {
                                uiState.current
                            } else {
                                stringResource(R.string.iosched_none)
                            }
                        } else {
                            stringResource(R.string.iosched_no_data)
                        },
                    )
                }
            }
        }

        item {
            Spacer(Modifier.height(20.dp))
        }
        val available = uiState.available
        if (!pageLoaded || !uiState.loaded || available.isEmpty()) {
            item {
                Box(
                    modifier = Modifier.fillMaxWidth().padding(vertical = 32.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        text = stringResource(R.string.iosched_no_data),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        } else {
            item {
                SegmentedColumn(title = stringResource(R.string.iosched_title)) {
                    available.forEach { name ->
                        item {
                            val isCurrent = name == uiState.current
                            SettingsBaseWidget(
                                iconPlaceholder = false,
                                title = name,
                                description = stringResource(
                                    if (isCurrent) R.string.iosched_in_use else R.string.iosched_switch_hint,
                                ),
                                onClick = { if (!isCurrent) viewModel.setScheduler(name) },
                                trailingContent = {
                                    if (isCurrent) {
                                        Icon(
                                            imageVector = Icons.TwoTone.Check,
                                            contentDescription = null,
                                            tint = MaterialTheme.colorScheme.primary,
                                        )
                                    }
                                },
                            )
                        }
                    }
                }
            }
        }
    }
}
