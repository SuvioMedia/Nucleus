package dev.nucleusframework.window.tao

import androidx.compose.ui.graphics.drawscope.DrawScope
import org.jetbrains.skia.ColorFilter
import org.jetbrains.skia.Image

/**
 * Draws an imported packed RGB texture without reading its pixels back to the CPU.
 *
 * Both arguments are borrowed for this draw only. Do not close or retain them; create
 * and close any derived shaders within the call. Apply [colorFilter] to the final paint
 * to preserve the producer's reference-white conversion in an HDR scene.
 * Planar YUV sources are not supported by this hook.
 */
public fun interface TextureViewImageRenderer {
    public fun DrawScope.drawFrame(
        image: Image,
        colorFilter: ColorFilter?,
    )
}
