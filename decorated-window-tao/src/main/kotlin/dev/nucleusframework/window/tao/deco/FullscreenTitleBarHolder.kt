package dev.nucleusframework.window.tao.deco

import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalContext
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

/**
 * Holds the title-bar rendering captured by the [TitleBar] composable when
 * `Modifier.newFullscreenControls()` is active and the window is fullscreen.
 *
 * [DecoratedWindow] reads from this holder and renders the bar as a sliding
 * overlay above the user content. [compositionLocalContext] preserves the
 * CompositionLocals from the original call site so user-provided values
 * (themes, etc.) remain accessible inside the overlay.
 *
 * Mirrors `decorated-window-jni`'s `FullscreenTitleBarHolder`.
 */
internal class FullscreenTitleBarHolder {
    var content: (@Composable () -> Unit)? by mutableStateOf(null)
    var titleBarHeight: Dp by mutableStateOf(0.dp)
    var revealInset: Dp by mutableStateOf(0.dp)
    var offsetY: Dp by mutableStateOf(0.dp)
    var compositionLocalContext: CompositionLocalContext? by mutableStateOf(null)
}

internal val LocalFullscreenTitleBarHolder = compositionLocalOf<FullscreenTitleBarHolder?> { null }

/**
 * Wraps user content and renders [holder] as a sliding title-bar overlay when
 * the window is fullscreen and the user opted in via `Modifier.newFullscreenControls()`.
 *
 * Pointer Y position near the top edge toggles visibility; the bar slides in
 * via [animateDpAsState]. Mirrors `decorated-window-jni`'s
 * `FullscreenTitleBarOverlay` and the pointer-tracking Box in `DecoratedWindow`.
 *
 * Pointer tracking uses [PointerEventPass.Initial] so it observes events
 * without consuming them — user content still receives all clicks.
 */
@Suppress("FunctionNaming")
@Composable
internal fun FullscreenOverlayHost(
    holder: FullscreenTitleBarHolder,
    isFullscreen: Boolean,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    var visible by remember { mutableStateOf(false) }
    val density = LocalDensity.current

    // Clear residual overlay content when leaving fullscreen so the next
    // composition starts from a clean slate (the TitleBar repopulates it).
    LaunchedEffect(isFullscreen) {
        if (!isFullscreen) {
            visible = false
            holder.content = null
            holder.revealInset = 0.dp
        }
    }

    val rootModifier =
        if (isFullscreen) {
            modifier.pointerInput(holder) {
                awaitPointerEventScope {
                    while (true) {
                        val event = awaitPointerEvent(PointerEventPass.Initial)
                        val y =
                            event.changes
                                .firstOrNull()
                                ?.position
                                ?.y ?: continue
                        val revealRegionPx =
                            with(density) {
                                (holder.titleBarHeight + holder.revealInset).toPx()
                            }
                        visible = y < revealRegionPx
                    }
                }
            }
        } else {
            modifier
        }

    val hasOverlay = isFullscreen && holder.content != null
    // macOS can temporarily take ownership of the top-edge pointer while its
    // system menu bar slides in. The native menu-bar monitor publishes that
    // reveal as [revealInset], so treat a non-zero inset as an independent
    // visibility signal instead of relying solely on Compose pointer events.
    val revealedByPlatformInset = holder.revealInset > 0.dp
    val offsetY by animateDpAsState(
        targetValue =
            if (hasOverlay && !visible && !revealedByPlatformInset) {
                -holder.titleBarHeight
            } else {
                0.dp
            },
        animationSpec = tween(durationMillis = 200),
    )
    SideEffect {
        if (holder.offsetY != offsetY) holder.offsetY = offsetY
    }

    Box(modifier = rootModifier) {
        content()

        if (hasOverlay) {
            val ctx = holder.compositionLocalContext
            Box(
                modifier =
                    Modifier
                        .align(Alignment.TopCenter)
                        .fillMaxWidth()
                        .offset(y = offsetY),
            ) {
                if (ctx != null) {
                    CompositionLocalProvider(ctx) {
                        holder.content?.invoke()
                    }
                } else {
                    holder.content?.invoke()
                }
            }
        }
    }
}
