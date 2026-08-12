package dev.nucleusframework.window.tao.scene

import androidx.compose.ui.InternalComposeUiApi
import androidx.compose.ui.graphics.asComposeCanvas
import androidx.compose.ui.scene.ComposeScene
import org.jetbrains.skia.BackendRenderTarget
import org.jetbrains.skia.ColorSpace
import org.jetbrains.skia.DirectContext
import org.jetbrains.skia.FramebufferFormat
import org.jetbrains.skia.Surface
import org.jetbrains.skia.SurfaceColorFormat
import org.jetbrains.skia.SurfaceOrigin

/**
 * Per-frame Skia/GL rendering helper shared by the overlay controller
 * and the popup scene layer. Wraps the default GL framebuffer in a
 * Skia [Surface], lets the scene paint, then presents.
 *
 * Caller must:
 *  1. have bound the surface's GL context (via the overlay/popup bridge),
 *  2. have called [DirectContext.resetGLAll] on [directContext] so Skia's
 *     GL state cache reflects reality after the external surface switch,
 *  3. provide [present] (the bridge's `nativeSwapBuffers`).
 */
@OptIn(InternalComposeUiApi::class)
internal inline fun renderGlFrame(
    widthPx: Int,
    heightPx: Int,
    directContext: DirectContext,
    scene: ComposeScene,
    clearColorArgb: Int,
    extendedDynamicRange: Boolean = false,
    framebufferId: Int = 0,
    crossinline afterFlush: () -> Unit = {},
    crossinline present: () -> Unit,
) {
    renderGlFrame(
        widthPx = widthPx,
        heightPx = heightPx,
        directContext = directContext,
        clearColorArgb = clearColorArgb,
        extendedDynamicRange = extendedDynamicRange,
        framebufferId = framebufferId,
        afterFlush = afterFlush,
        present = present,
    ) { canvas, nanoTime ->
        scene.render(canvas.asComposeCanvas(), nanoTime)
    }
}

@OptIn(InternalComposeUiApi::class)
internal inline fun renderGlFrame(
    widthPx: Int,
    heightPx: Int,
    directContext: DirectContext,
    clearColorArgb: Int,
    extendedDynamicRange: Boolean = false,
    framebufferId: Int = 0,
    crossinline afterFlush: () -> Unit = {},
    crossinline present: () -> Unit,
    crossinline render: (org.jetbrains.skia.Canvas, Long) -> Unit,
) {
    if (widthPx <= 0 || heightPx <= 0) return
    val rt =
        BackendRenderTarget.makeGL(
            width = widthPx,
            height = heightPx,
            sampleCnt = 0,
            stencilBits = 8,
            fbId = framebufferId,
            fbFormat =
                if (extendedDynamicRange) {
                    FramebufferFormat.GR_GL_RGBA16F
                } else {
                    FramebufferFormat.GR_GL_RGBA8
                },
        )
    val surface =
        Surface.makeFromBackendRenderTarget(
            context = directContext,
            rt = rt,
            origin = SurfaceOrigin.BOTTOM_LEFT,
            colorFormat =
                if (extendedDynamicRange) {
                    skikoRgbaF16SurfaceColorFormat
                } else {
                    SurfaceColorFormat.RGBA_8888
                },
            colorSpace = if (extendedDynamicRange) ColorSpace.sRGBLinear else ColorSpace.sRGB,
        ) ?: run {
            rt.close()
            return
        }
    try {
        surface.canvas.clear(clearColorArgb)
        render(surface.canvas, System.nanoTime())
        surface.flushAndSubmit(syncCpu = false)
        afterFlush()
        present()
    } finally {
        surface.close()
        rt.close()
    }
}
