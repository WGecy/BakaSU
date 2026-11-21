package org.bakasu.bakasu.ui.screen.toolbox

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.LargeFlexibleTopAppBar
import androidx.compose.material3.LoadingIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.PrimaryScrollableTabRow
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRowDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberTopAppBarState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.bakasu.bakasu.R
import org.bakasu.bakasu.data.shell.KsuCliRepository
import org.bakasu.bakasu.ui.component.HorizontalPagerWithInteraction
import org.bakasu.bakasu.ui.component.settings.AppBackButton
import org.bakasu.bakasu.ui.navigation.LocalNavigator
import org.bakasu.bakasu.ui.theme.CardConfig
import org.bakasu.bakasu.ui.theme.ThemeConfig
import org.bakasu.bakasu.ui.theme.blurEffect
import org.bakasu.bakasu.ui.theme.blurSource
import org.koin.compose.koinInject

@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun ToolboxScreen() {
    val navigator = LocalNavigator.current
    val themeConfig: ThemeConfig = koinInject()
    val cardConfig: CardConfig = koinInject()
    val ksuCliRepository: KsuCliRepository = koinInject()
    val topAppBarState = rememberTopAppBarState()
    val scrollBehavior = TopAppBarDefaults.exitUntilCollapsedScrollBehavior(topAppBarState)
    val coroutineScope = rememberCoroutineScope()

    var rootState by remember { mutableStateOf<Boolean?>(null) }
    LaunchedEffect(Unit) {
        rootState = withContext(Dispatchers.IO) {
            runCatching { ksuCliRepository.rootAvailable() }.getOrDefault(false)
        }
    }
    val rootAvailable = rootState == true

    val subpages = if (rootAvailable) {
        listOf(
            ToolboxSubpage(
                title = stringResource(R.string.iosched_title),
            ) { innerPadding, nestedScrollConnection ->
                IoSchedulerTab(
                    innerPadding = innerPadding,
                    nestedScrollConnection = nestedScrollConnection,
                )
            },
            ToolboxSubpage(
                title = stringResource(R.string.settings_folk_mount),
            ) { innerPadding, nestedScrollConnection ->
                FolkMountTab(
                    innerPadding = innerPadding,
                    nestedScrollConnection = nestedScrollConnection,
                )
            },
            ToolboxSubpage(
                title = stringResource(R.string.builtin_zygisk_tab_title),
            ) { innerPadding, nestedScrollConnection ->
                BuiltinZygiskTab(
                    innerPadding = innerPadding,
                    nestedScrollConnection = nestedScrollConnection,
                )
            },
        )
    } else {
        emptyList()
    }

    val pagerState = rememberPagerState(
        initialPage = 0,
        pageCount = { subpages.size },
    )

    val chromeColor =
        if (themeConfig.isEnableBlur) {
            Color.Transparent
        } else {
            MaterialTheme.colorScheme.surfaceContainer.copy(cardConfig.cardAlpha)
        }

    Scaffold(
        topBar = {
            Column(modifier = Modifier.blurEffect()) {
                LargeFlexibleTopAppBar(
                    title = { Text(stringResource(R.string.toolbox_title)) },
                    scrollBehavior = scrollBehavior,
                    navigationIcon = {
                        AppBackButton(
                            onClick = { navigator.pop() },
                        )
                    },
                    colors = TopAppBarDefaults.topAppBarColors().copy(
                        containerColor = chromeColor,
                        scrolledContainerColor = chromeColor,
                    ),
                    windowInsets = TopAppBarDefaults.windowInsets.add(WindowInsets(left = 12.dp)),
                )

                if (subpages.isNotEmpty()) {
                    PrimaryScrollableTabRow(
                        selectedTabIndex = pagerState.currentPage,
                        containerColor = chromeColor,
                        edgePadding = 0.dp,
                        minTabWidth = 0.dp,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        subpages.forEachIndexed { index, subpage ->
                            Tab(
                                selected = pagerState.currentPage == index,
                                onClick = {
                                    coroutineScope.launch {
                                        pagerState.animateScrollToPage(index)
                                    }
                                },
                                modifier = Modifier.widthIn(
                                    min = TabRowDefaults.ScrollableTabRowMinTabWidth,
                                ),
                                unselectedContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
                                text = { Text(subpage.title) },
                            )
                        }
                    }
                }
            }
        },
        containerColor = Color.Transparent,
        modifier = Modifier.fillMaxSize(),
    ) { innerPadding ->
        when {
            rootState == null -> Box(
                modifier = Modifier.fillMaxSize().padding(innerPadding),
                contentAlignment = Alignment.Center,
            ) {
                LoadingIndicator()
            }

            subpages.isEmpty() -> Box(
                modifier = Modifier.fillMaxSize().padding(innerPadding),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = stringResource(R.string.root_required),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            else -> HorizontalPagerWithInteraction(
                modifier = Modifier
                    .fillMaxSize()
                    .blurSource(),
                state = pagerState,
            ) { page ->
                subpages[page].content(innerPadding, scrollBehavior.nestedScrollConnection)
            }
        }
    }
}

@Composable
internal fun rememberToolboxPageLoaded(load: suspend () -> Unit): Boolean {
    var loaded by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        delay(400)
        load()
        loaded = true
    }
    return loaded
}

private class ToolboxSubpage(
    val title: String,
    val content: @Composable (PaddingValues, NestedScrollConnection) -> Unit,
)
