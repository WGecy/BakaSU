package org.bakasu.bakasu.ui.theme

import androidx.compose.animation.core.FiniteAnimationSpec
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween

object FolkMotion {
    const val PressedScale = 0.97f

    val PressScale: FiniteAnimationSpec<Float> = spring(dampingRatio = 0.7f, stiffness = 700f)

    val PressDown: FiniteAnimationSpec<Float> = tween(durationMillis = 80)

    fun <T> smoothSpring(): FiniteAnimationSpec<T> = spring(dampingRatio = Spring.DampingRatioNoBouncy, stiffness = 250f)
}
