package dev.nucleusframework.window.tao.scene

import org.jetbrains.skia.ColorType
import org.jetbrains.skia.SurfaceColorFormat

/**
 * Skiko 0.150.1 forwards `SurfaceColorFormat.ordinal` to native Skia as a
 * `SkColorType`, but its `SurfaceColorFormat` enum omits several modern color
 * types before F16. Consequently the enum constant named `RGBA_F16` carries an
 * obsolete ordinal and Skia rejects a real `MTLPixelFormatRGBA16Float` target.
 *
 * `ColorType` does mirror the current `SkColorType` table. Selecting the
 * `SurfaceColorFormat` entry at that ordinal preserves the ABI expected by the
 * native bridge. Keep this compatibility shim in one place until Skiko exposes
 * a Metal backend-texture API or fixes the mapping.
 */
internal val skikoRgbaF16SurfaceColorFormat: SurfaceColorFormat =
    checkNotNull(SurfaceColorFormat.entries.getOrNull(ColorType.RGBA_F16.ordinal)) {
        "Skiko SurfaceColorFormat cannot represent the native RGBA_F16 ordinal"
    }
