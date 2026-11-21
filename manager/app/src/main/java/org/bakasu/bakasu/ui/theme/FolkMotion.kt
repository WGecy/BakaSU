package org.bakasu.bakasu.ui.theme

import androidx.compose.animation.core.FiniteAnimationSpec
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.VisibilityThreshold
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.ui.unit.IntSize

object FolkMotion {
    const val PressedScale = 0.97f

    val PressScale: FiniteAnimationSpec<Float> = spring(dampingRatio = 0.7f, stiffness = 700f)

    val PressDown: FiniteAnimationSpec<Float> = tween(durationMillis = 80)

    fun <T> smoothSpring(): FiniteAnimationSpec<T> = spring(dampingRatio = Spring.DampingRatioNoBouncy, stiffness = 250f)

    val ScrollIntoView: FiniteAnimationSpec<Float> = tween(durationMillis = 80)

    val FadeInOut: FiniteAnimationSpec<Float> = tween(durationMillis = 200)

    val IconRotation: FiniteAnimationSpec<Float> = tween(durationMillis = 300)

    val SlotSize: FiniteAnimationSpec<IntSize> = spring(
        dampingRatio = Spring.DampingRatioNoBouncy,
        stiffness = 900f,
        visibilityThreshold = IntSize.VisibilityThreshold,
    )

    val SlotFade: FiniteAnimationSpec<Float> = tween(durationMillis = 140)

    val SlotScale: FiniteAnimationSpec<Float> = spring(dampingRatio = 0.8f, stiffness = 900f)
}
