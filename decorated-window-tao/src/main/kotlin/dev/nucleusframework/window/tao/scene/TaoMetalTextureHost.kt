package dev.nucleusframework.window.tao.scene

import androidx.compose.runtime.ProvidableCompositionLocal
import androidx.compose.runtime.State
import androidx.compose.runtime.compositionLocalOf
import dev.nucleusframework.window.tao.TextureViewHostCapabilities
import dev.nucleusframework.window.tao.UnavailableTextureViewHostCapabilitiesState
import dev.nucleusframework.window.tao.ffi.NativeMetalBridge
import org.jetbrains.skia.DirectContext

/**
 * macOS: the Metal/Skia surface the enclosing Compose scene draws into.
 * Consumed by the `TextureView` composable, which must build its Skia objects
 * on the very context that will replay the recorded scene — otherwise Skia
 * silently drops the draw (a GPU image belongs to exactly one `DirectContext`).
 *
 * Unlike Windows, where host, popups and overlays all share one Skia context,
 * every macOS surface owns its own `DirectContext` on its own render thread.
 * Each surface therefore provides its own [LocalTaoMetalTextureHost]:
 * [TaoComposeSceneHost] for the window scene,
 * [dev.nucleusframework.window.tao.popup.TaoPopupSceneLayer] for native popup
 * layers.
 *
 * Threading: [metalDevicePtr] / [directContext] are read on the macOS main
 * thread; every *use* of [directContext] must be wrapped in
 * [runOnRenderThread] (Skia's Metal context is thread-affine).
 */
internal interface TaoMetalTextureHost {
    val textureViewHostCapabilities: State<TextureViewHostCapabilities>
        get() = UnavailableTextureViewHostCapabilitiesState

    /** `id<MTLDevice>` [directContext] renders with. */
    val metalDevicePtr: Long

    /** `id<MTLCommandQueue>` used by [directContext]. */
    val metalCommandQueuePtr: Long

    /** Borrowed `NSView` pointer whose screen owns this composition surface. */
    val nativeViewPtr: Long

    /** Skia context of this surface; only touch it inside [runOnRenderThread]. */
    val directContext: DirectContext

    /**
     * Runs [block] on the render thread owning [directContext] and blocks
     * until it returns. Safe from the main thread during composition,
     * disposal, and the record pass — the render thread is idle at those
     * points (see [TaoComposeSceneHost]'s lifetime invariant).
     */
    fun <T> runOnRenderThread(block: () -> T): T
}

/**
 * The [TaoMetalTextureHost] of one macOS surface, built on first use and cached.
 *
 * Every surface that can host a `TextureView` — window scene, tray panel,
 * `NativeView` overlay — resolves it from the same three things, in the same
 * order: its Skia context, its live Metal attachment, and the `id<MTLDevice>`
 * that attachment renders with. Any of them missing means the surface is not up
 * yet (or is being torn down) and the composable degrades to an empty `Box`.
 *
 * The instance must be **stable**: the import registry keys on it, so a value
 * rebuilt on every read would re-key every `TextureView`'s `remember` on every
 * recomposition. Surfaces drop it with [invalidate] when they close their
 * context, so the next attach builds a fresh one.
 *
 * Only [create] stays with the caller — it is the one part that differs, since
 * each surface reaches its render thread its own way (a popup layer hops through
 * its parent host, a panel through itself). Main thread only.
 */
internal class MetalTextureHostCache {
    private var cached: TaoMetalTextureHost? = null

    fun get(
        attachmentHandle: Long,
        directContext: DirectContext?,
        create: (
            metalDevicePtr: Long,
            metalCommandQueuePtr: Long,
            nativeViewPtr: Long,
            context: DirectContext,
        ) -> TaoMetalTextureHost,
    ): TaoMetalTextureHost? {
        cached?.let { return it }
        val context = directContext ?: return null
        if (attachmentHandle == 0L) return null
        val device = NativeMetalBridge.nativeDevicePtr(attachmentHandle)
        val commandQueue = NativeMetalBridge.nativeQueuePtr(attachmentHandle)
        val nativeView = NativeMetalBridge.nativeViewPtr(attachmentHandle)
        if (device == 0L || commandQueue == 0L || nativeView == 0L) return null
        return create(device, commandQueue, nativeView, context).also { cached = it }
    }

    /** Drops the cached instance — called when the surface closes its context. */
    fun invalidate() {
        cached = null
    }
}

internal val LocalTaoMetalTextureHost: ProvidableCompositionLocal<TaoMetalTextureHost?> =
    compositionLocalOf { null }
