package org.bakasu.bakasu.ui.component.material

import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItemColors
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.ListItemShapes
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.LocalRippleConfiguration
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MaterialTheme.colorScheme
import androidx.compose.material3.ProvideTextStyle
import androidx.compose.material3.RippleConfiguration
import androidx.compose.material3.SegmentedListItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Outline
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.layout.layout
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.zIndex
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlinx.coroutines.launch
import org.bakasu.bakasu.ui.theme.CardConfig
import org.bakasu.bakasu.ui.theme.FolkMotion
import org.bakasu.bakasu.ui.theme.ThemeConfig
import org.bakasu.bakasu.ui.theme.renderBackgroundBlur
import org.bakasu.bakasu.ui.util.LocalBackgroundBlurProvided
import org.bakasu.bakasu.wallpaper.LocalFolkWallpaperTokens
import org.koin.compose.koinInject

val LocalListItemShapes = compositionLocalOf<ListItemShapes?> { null }
private val SegmentedOuterRadius = 16.dp
private val SegmentedInnerRadius = 4.dp
private const val SegmentedSpringStiffness = 800f
private const val SegmentedSpringDamping = 0.9f

private val LocalWallpaperGroupActive = staticCompositionLocalOf { false }

@Suppress("ktlint:compose:content-slot-reused")
@Composable
private fun WallpaperGroupBackground(
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    val tokens = LocalFolkWallpaperTokens.current
    val themeConfig = koinInject<ThemeConfig>()
    if (tokens == null) {
        if (!themeConfig.isEnableBlurExp) {
            content()
            return
        }
        Box(
            modifier = modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(SegmentedOuterRadius))
                .renderBackgroundBlur(),
        ) {
            CompositionLocalProvider(LocalBackgroundBlurProvided provides true) {
                content()
            }
        }
        return
    }
    Box(
        modifier = modifier
            .fillMaxWidth()
            .background(
                color = tokens.group.fill,
                shape = RoundedCornerShape(SegmentedOuterRadius),
            ),
    ) {
        CompositionLocalProvider(LocalWallpaperGroupActive provides true) {
            content()
        }
    }
}

@DslMarker
annotation class SegmentedColumnDsl

@Composable
private fun defaultSegmentedColors(): ListItemColors {
    val material = LocalFolkWallpaperTokens.current?.group
    if (material != null) {
        val inGroup = LocalWallpaperGroupActive.current
        return ListItemDefaults.segmentedColors(
            containerColor = if (inGroup) Color.Transparent else material.fill,
            contentColor = material.contentColor,
            disabledContainerColor = if (inGroup) Color.Transparent else material.fill,
            supportingContentColor = material.supportingColor,
        )
    }
    val cardConfig = koinInject<CardConfig>()
    val themeConfig = koinInject<ThemeConfig>()
    val containerColor = if (themeConfig.isEnableBlurExp && LocalBackgroundBlurProvided.current) {
        Color.Transparent
    } else {
        colorScheme.surfaceBright.copy(alpha = cardConfig.cardAlpha)
    }
    return ListItemDefaults.segmentedColors(
        containerColor = containerColor,
        disabledContainerColor = containerColor,
        supportingContentColor = colorScheme.onSurfaceVariant,
    )
}

@Composable
private fun selectionRowColors(colors: ListItemColors): ListItemColors {
    if (LocalFolkWallpaperTokens.current == null) return colors
    return colors.copy(
        selectedContainerColor = colors.containerColor,
        selectedContentColor = colors.contentColor,
        selectedLeadingContentColor = colors.leadingContentColor,
        selectedTrailingContentColor = colors.trailingContentColor,
        selectedOverlineContentColor = colors.overlineContentColor,
        selectedSupportingContentColor = colors.supportingContentColor,
    )
}

@Composable
private fun defaultSingleSegmentedShape(index: Int, count: Int): ListItemShapes {
    val base = ListItemDefaults.segmentedShapes(index, count)
    if (LocalFolkWallpaperTokens.current != null) {
        val shape = RoundedCornerShape(
            topStart = if (index == 0) SegmentedOuterRadius else 0.dp,
            topEnd = if (index == 0) SegmentedOuterRadius else 0.dp,
            bottomStart = if (index == count - 1) SegmentedOuterRadius else 0.dp,
            bottomEnd = if (index == count - 1) SegmentedOuterRadius else 0.dp,
        )
        return base.copy(shape = shape)
    }
    return if (count == 1) {
        base.copy(shape = MaterialTheme.shapes.large)
    } else {
        base
    }
}

@Composable
fun SegmentedColumn(
    modifier: Modifier = Modifier,
    title: String = "",
    visibleLen: Int = 0,
    content: List<@Composable () -> Unit>,
) {
    if (content.isEmpty()) return

    Column(modifier = modifier) {
        if (title.isNotEmpty()) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleSmall,
                color = colorScheme.primary,
                modifier = Modifier.padding(start = 16.dp, bottom = 8.dp),
            )
        }
        WallpaperGroupBackground {
            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                content.forEachIndexed { index, itemContent ->
                    CompositionLocalProvider(
                        LocalListItemShapes provides defaultSingleSegmentedShape(
                            index = index,
                            count = if (visibleLen > 0) visibleLen else content.size,
                        ),
                    ) {
                        itemContent()
                    }
                }
            }
        }
    }
}

@SegmentedColumnDsl
class SegmentedColumnScope {
    internal data class Entry(
        val key: Any?,
        val visible: Boolean,
        val content: @Composable () -> Unit,
    )

    internal val entries = mutableListOf<Entry>()

    fun item(
        key: Any? = null,
        visible: Boolean = true,
        content: @Composable () -> Unit,
    ) {
        entries.add(Entry(key ?: entries.size, visible, content))
    }
}

@Composable
fun SegmentedColumn(
    modifier: Modifier = Modifier,
    title: String = "",
    content: SegmentedColumnScope.() -> Unit,
) {
    val entries = SegmentedColumnScope().apply(content).entries
    if (entries.isEmpty()) return

    Column(modifier = modifier) {
        if (title.isNotEmpty()) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleSmall,
                color = colorScheme.primary,
                modifier = Modifier.padding(start = 16.dp, bottom = 8.dp),
            )
        }

        val dpSpring = spring<Dp>(SegmentedSpringDamping, SegmentedSpringStiffness)

        val progresses = entries.mapIndexed { index, entry ->
            key(entry.key ?: index) {
                animateFloatAsState(
                    targetValue = if (entry.visible) 1f else 0f,
                    animationSpec = FolkMotion.smoothSpring(),
                    label = "SegmentedProgress",
                )
            }
        }

        val visibilityState = rememberUpdatedState(entries.map { it.visible })
        val progressesState = rememberUpdatedState(progresses)

        val firstVisible = entries.indexOfFirst { it.visible }
        val lastVisible = entries.indexOfLast { it.visible }

        WallpaperGroupBackground {
            Layout(
                content = {
                    entries.forEachIndexed { index, entry ->
                        key(entry.key ?: index) {
                            val isFirst = if (firstVisible == -1) index == 0 else index == firstVisible
                            val isLast = if (lastVisible == -1) index == entries.lastIndex else index == lastVisible

                            val topRadius by animateDpAsState(
                                if (isFirst) SegmentedOuterRadius else SegmentedInnerRadius,
                                dpSpring,
                                label = "SegmentedTopRadius",
                            )
                            val bottomRadius by animateDpAsState(
                                if (isLast) SegmentedOuterRadius else SegmentedInnerRadius,
                                dpSpring,
                                label = "SegmentedBottomRadius",
                            )
                            val gap by animateDpAsState(
                                if (isFirst) 0.dp else ListItemDefaults.SegmentedGap,
                                dpSpring,
                                label = "SegmentedGap",
                            )

                            val shape = RoundedCornerShape(
                                topStart = topRadius,
                                topEnd = topRadius,
                                bottomStart = bottomRadius,
                                bottomEnd = bottomRadius,
                            )

                            Box(
                                modifier = Modifier
                                    .zIndex(if (entry.visible) (entries.size - index).toFloat() else -index.toFloat())
                                    .pointerInput(entry.key ?: index) {
                                        awaitEachGesture {
                                            val down = awaitFirstDown(
                                                requireUnconsumed = false,
                                                pass = PointerEventPass.Initial,
                                            )
                                            val visibleNow = visibilityState.value.getOrNull(index) ?: false
                                            val progressNow = progressesState.value.getOrNull(index)?.value ?: 0f
                                            val opening = progressNow != if (visibleNow) 1f else 0f
                                            if (!visibleNow || opening) {
                                                down.consume()
                                                do {
                                                    val event = awaitPointerEvent(PointerEventPass.Initial)
                                                    event.changes.forEach { it.consume() }
                                                } while (event.changes.any { it.pressed })
                                            }
                                        }
                                    }
                                    .graphicsLayer {
                                        val progress = progresses[index].value.coerceAtLeast(0f)
                                        clip = true
                                        this.shape = object : Shape {
                                            override fun createOutline(
                                                size: Size,
                                                layoutDirection: LayoutDirection,
                                                density: Density,
                                            ): Outline = Outline.Rectangle(Rect(0f, 0f, size.width, size.height * progress))
                                        }
                                        alpha = (progress * 1.5f).coerceIn(0f, 1f)
                                    },
                            ) {
                                CompositionLocalProvider(
                                    LocalListItemShapes provides ListItemDefaults.segmentedShapes(0, 1).copy(shape = shape),
                                ) {
                                    Column(modifier = Modifier.padding(top = gap.coerceAtLeast(0.dp))) {
                                        entry.content()
                                    }
                                }
                            }
                        }
                    }
                },
            ) { measurables, constraints ->
                val placeables = measurables.map { it.measure(constraints) }
                val positions = IntArray(placeables.size)
                var y = 0f
                placeables.forEachIndexed { index, placeable ->
                    positions[index] = y.roundToInt()
                    y += placeable.height * progresses[index].value.coerceAtLeast(0f)
                }
                layout(constraints.maxWidth, y.roundToInt().coerceAtLeast(0)) {
                    placeables.forEachIndexed { index, placeable ->
                        if (progresses[index].value > 0f) {
                            placeable.placeRelative(0, positions[index])
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SegmentedRow(
    modifier: Modifier = Modifier,
    clickable: Boolean,
    enabled: Boolean,
    interactionSource: MutableInteractionSource?,
    content: @Composable (Modifier, MutableInteractionSource) -> Unit,
) {
    val pressedSource = interactionSource ?: remember { MutableInteractionSource() }
    val rowModifier = if (clickable) modifier.folkPressScale(pressedSource, enabled) else modifier
    val rippleConfiguration = if (clickable) {
        RippleConfiguration(color = Color.Transparent)
    } else {
        LocalRippleConfiguration.current
    }
    CompositionLocalProvider(LocalRippleConfiguration provides rippleConfiguration) {
        content(rowModifier, pressedSource)
    }
}

@Composable
fun SegmentedListItem(
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null,
    onLongClick: (() -> Unit)? = null,
    enabled: Boolean = true,
    colors: ListItemColors = defaultSegmentedColors(),
    interactionSource: MutableInteractionSource? = null,
    headlineContent: @Composable () -> Unit,
    overlineContent: @Composable (() -> Unit)? = null,
    supportingContent: @Composable (() -> Unit)? = null,
    leadingContent: @Composable (() -> Unit)? = null,
    trailingContent: @Composable (() -> Unit)? = null,
) {
    val tokens = LocalFolkWallpaperTokens.current
    val source = interactionSource ?: remember { MutableInteractionSource() }
    SegmentedRow(
        modifier = modifier,
        clickable = onClick != null,
        enabled = enabled,
        interactionSource = source,
    ) { rowModifier, source ->
        SegmentedListItem(
            onClick = onClick ?: {},
            onLongClick = onLongClick,
            enabled = enabled,
            colors = colors,
            interactionSource = source,
            shapes = LocalListItemShapes.current ?: ListItemDefaults.segmentedShapes(0, 1),
            modifier = rowModifier,
            leadingContent = leadingContent,
            trailingContent = trailingContent,
            overlineContent = overlineContent,
            supportingContent = supportingContent,
            verticalAlignment = Alignment.CenterVertically,
            content = headlineContent,
        )
    }
}

@Composable
fun SegmentedListItem(
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    colors: ListItemColors = defaultSegmentedColors(),
    interactionSource: MutableInteractionSource? = null,
    headlineContent: @Composable () -> Unit,
    overlineContent: @Composable (() -> Unit)? = null,
    supportingContent: @Composable (() -> Unit)? = null,
    leadingContent: @Composable (() -> Unit)? = null,
    trailingContent: @Composable (() -> Unit)? = null,
    onLongClick: (() -> Unit)? = null,
) {
    SegmentedRow(
        modifier = modifier,
        clickable = true,
        enabled = enabled,
        interactionSource = interactionSource,
    ) { rowModifier, source ->
        SegmentedListItem(
            checked = checked,
            onCheckedChange = onCheckedChange,
            shapes = LocalListItemShapes.current ?: ListItemDefaults.segmentedShapes(0, 1),
            modifier = rowModifier,
            enabled = enabled,
            colors = selectionRowColors(colors),
            interactionSource = source,
            leadingContent = leadingContent,
            trailingContent = trailingContent,
            overlineContent = overlineContent,
            supportingContent = supportingContent,
            verticalAlignment = Alignment.CenterVertically,
            onLongClick = onLongClick,
            content = headlineContent,
        )
    }
}

@Composable
fun SegmentedListItem(
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    colors: ListItemColors = defaultSegmentedColors(),
    interactionSource: MutableInteractionSource? = null,
    headlineContent: @Composable () -> Unit,
    overlineContent: @Composable (() -> Unit)? = null,
    supportingContent: @Composable (() -> Unit)? = null,
    leadingContent: @Composable (() -> Unit)? = null,
    trailingContent: @Composable (() -> Unit)? = null,
    onLongClick: (() -> Unit)? = null,
) {
    val tokens = LocalFolkWallpaperTokens.current
    val source = interactionSource ?: remember { MutableInteractionSource() }
    SegmentedRow(
        modifier = modifier,
        clickable = true,
        enabled = enabled,
        interactionSource = source,
    ) { rowModifier, source ->
        SegmentedListItem(
            selected = selected,
            onClick = onClick,
            shapes = LocalListItemShapes.current ?: ListItemDefaults.segmentedShapes(0, 1),
            modifier = rowModifier,
            enabled = enabled,
            colors = selectionRowColors(colors),
            interactionSource = source,
            leadingContent = leadingContent,
            trailingContent = trailingContent,
            overlineContent = overlineContent,
            supportingContent = supportingContent,
            verticalAlignment = Alignment.CenterVertically,
            onLongClick = onLongClick,
            content = headlineContent,
        )
    }
}

private const val SlideTravelMultiplier = 3f

@Composable
fun SegmentedSwitchItem(
    icon: ImageVector? = null,
    title: String,
    summary: String? = null,
    colors: ListItemColors = defaultSegmentedColors(),
    checked: Boolean,
    enabled: Boolean = true,
    requireStationaryTap: Boolean = true,
    onCheckedChange: (Boolean) -> Unit,
) {
    val haptic = LocalHapticFeedback.current
    val interactionSource = remember { MutableInteractionSource() }
    val dragged = remember { mutableStateOf(false) }
    val guard = if (requireStationaryTap) {
        Modifier.pointerInput(enabled, title) {
            val slideSlop = viewConfiguration.touchSlop * SlideTravelMultiplier
            awaitEachGesture {
                val down = awaitFirstDown(requireUnconsumed = false)
                var pull = 0f
                var vertical = 0f
                while (true) {
                    val event = awaitPointerEvent(PointerEventPass.Final)
                    val change = event.changes.firstOrNull { it.id == down.id } ?: break
                    val delta = change.position - down.position
                    pull = maxOf(pull, delta.getDistance())
                    vertical = maxOf(vertical, abs(delta.y))
                    if (pull > slideSlop || vertical > viewConfiguration.touchSlop) {
                        dragged.value = true
                    }
                    if (!change.pressed) break
                }
                dragged.value = false
            }
        }
    } else {
        Modifier
    }

    SegmentedListItem(
        onClick = {
            if (!(requireStationaryTap && dragged.value)) {
                haptic.performHapticFeedback(HapticFeedbackType.VirtualKey)
                onCheckedChange(!checked)
            }
        },
        modifier = guard,
        enabled = enabled,
        interactionSource = interactionSource,
        colors = colors,
        headlineContent = { Text(title) },
        leadingContent = icon?.let { { Icon(it, title) } },
        trailingContent = {
            ExpressiveSwitch(
                checked = checked,
                enabled = enabled,
                onCheckedChange = null,
                interactionSource = interactionSource,
            )
        },
        supportingContent = summary?.let { { Text(it) } },
    )
}

@Composable
fun SegmentedTextField(
    modifier: Modifier = Modifier,
    label: String = "",
    value: String,
    onValueChange: (String) -> Unit,
    enabled: Boolean = true,
    readOnly: Boolean = false,
    colors: ListItemColors = defaultSegmentedColors(),
    textStyle: TextStyle = MaterialTheme.typography.bodyLarge,
    keyboardOptions: KeyboardOptions = KeyboardOptions.Default,
    keyboardActions: KeyboardActions = KeyboardActions.Default,
    singleLine: Boolean = false,
    maxLines: Int = if (singleLine) 1 else Int.MAX_VALUE,
    minLines: Int = 1,
    visualTransformation: VisualTransformation = VisualTransformation.None,
    onTextLayout: (TextLayoutResult) -> Unit = {},
    interactionSource: MutableInteractionSource = remember { MutableInteractionSource() },
    cursorBrush: Brush = SolidColor(colorScheme.primary),
    placeholder: @Composable (() -> Unit)? = { Text("-") },
    leadingContent: @Composable (() -> Unit)? = null,
    trailingContent: @Composable (() -> Unit)? = null,
    supportingContent: @Composable (() -> Unit)? = null,
    isError: Boolean = false,
) {
    val bringIntoViewRequester = remember { BringIntoViewRequester() }
    val coroutineScope = rememberCoroutineScope()
    val focusRequester = remember { FocusRequester() }

    SegmentedListItem(
        modifier = modifier
            .bringIntoViewRequester(bringIntoViewRequester)
            .focusRequester(focusRequester),
        colors = colors,
        onClick = { focusRequester.requestFocus() },
        leadingContent = leadingContent,
        supportingContent = supportingContent,
        trailingContent = trailingContent,
        headlineContent = {
            Column {
                if (label.isNotEmpty()) {
                    Text(text = label, color = if (isError) colorScheme.error else colors.contentColor)
                }
                BasicTextField(
                    value = value,
                    onValueChange = onValueChange,
                    modifier = Modifier
                        .fillMaxWidth()
                        .focusRequester(focusRequester)
                        .onFocusChanged {
                            if (it.isFocused) {
                                coroutineScope.launch {
                                    bringIntoViewRequester.bringIntoView()
                                }
                            }
                        },
                    enabled = enabled,
                    readOnly = readOnly,
                    textStyle = textStyle.copy(
                        colors.supportingContentColor,
                        fontSize = MaterialTheme.typography.bodyMedium.fontSize,
                        lineHeight = MaterialTheme.typography.bodyMedium.lineHeight,
                    ),
                    keyboardOptions = keyboardOptions,
                    keyboardActions = keyboardActions,
                    singleLine = singleLine,
                    maxLines = maxLines,
                    minLines = minLines,
                    visualTransformation = visualTransformation,
                    onTextLayout = onTextLayout,
                    interactionSource = interactionSource,
                    cursorBrush = cursorBrush,
                    decorationBox = { innerTextField ->
                        if (value.isEmpty() && placeholder != null) {
                            Box(contentAlignment = Alignment.CenterStart) {
                                CompositionLocalProvider(
                                    LocalContentColor provides colors.supportingContentColor,
                                ) {
                                    ProvideTextStyle(value = MaterialTheme.typography.bodyMedium) {
                                        placeholder()
                                    }
                                }
                            }
                        }
                        innerTextField()
                    },
                )
            }
        },
    )
}
