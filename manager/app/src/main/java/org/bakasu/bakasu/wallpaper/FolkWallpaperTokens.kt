package org.bakasu.bakasu.wallpaper

import androidx.compose.runtime.Immutable
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.compositeOver

enum class WallpaperSurfaceRole { Group, Raised, Overlay }

@Immutable
data class WallpaperMaterial(
    val tint: Color,
    val alpha: Float,
    val contentColor: Color,
    val supportingColor: Color,
) {
    val fill: Color get() = tint.copy(alpha = alpha)
}

@Immutable
data class FolkWallpaperTokens(
    val group: WallpaperMaterial,
    val raised: WallpaperMaterial,
    val overlay: WallpaperMaterial,
    val chrome: WallpaperMaterial,
    val selection: WallpaperMaterial,
) {
    fun material(role: WallpaperSurfaceRole, minimumAlpha: Float = 0f): WallpaperMaterial {
        val material = when (role) {
            WallpaperSurfaceRole.Group -> group
            WallpaperSurfaceRole.Raised -> raised
            WallpaperSurfaceRole.Overlay -> overlay
        }
        return if (material.alpha >= minimumAlpha) {
            material
        } else {
            material.copy(alpha = minimumAlpha.coerceIn(0f, 1f))
        }
    }

    fun raisedOverChrome(): Color {
        val under = chrome.alpha
        val alpha = if (under >= 1f) 0f else (raised.alpha - under) / (1f - under)
        return raised.tint.copy(alpha = alpha.coerceIn(0f, 1f))
    }

    fun chromeControlFill(tint: Color): Color {
        val progress = ((chrome.alpha - CHROME_ALPHA_FLOOR) / (1f - CHROME_ALPHA_FLOOR))
            .coerceIn(0f, 1f)
        val veil = chrome.contentColor.copy(alpha = CHROME_CONTROL_VEIL_ALPHA * (1f - progress))
        return veil.compositeOver(tint.copy(alpha = chrome.alpha))
    }
}

val LocalFolkWallpaperTokens = compositionLocalOf<FolkWallpaperTokens?> { null }

private const val CHROME_ALPHA_FLOOR = 0.10f

private const val CHROME_CONTROL_VEIL_ALPHA = 0.12f
