@file:Suppress("MatchingDeclarationName")

package dev.nucleusframework.window.tao

import androidx.compose.foundation.layout.Box
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.layout.ContentScale
import dev.nucleusframework.core.runtime.Platform
import dev.nucleusframework.window.tao.ffi.NativeTaoMacOsTextureBridge
import dev.nucleusframework.window.tao.scene.LocalTaoMetalTextureHost
import dev.nucleusframework.window.tao.scene.TaoMetalTextureHost
import dev.nucleusframework.window.tao.scene.skikoRgbaF16SurfaceColorFormat
import org.jetbrains.skia.BackendRenderTarget
import org.jetbrains.skia.ColorSpace
import org.jetbrains.skia.ContentChangeMode
import org.jetbrains.skia.Image
import org.jetbrains.skia.Rect
import org.jetbrains.skia.Surface
import org.jetbrains.skia.SurfaceColorFormat
import org.jetbrains.skia.SurfaceOrigin
import java.util.ArrayDeque
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Raw Metal objects backing the current macOS Tao composition surface.
 *
 * A same-process producer may submit its writes through [commandQueue] before
 * calling [TextureViewController.markFrameAvailable]. Nucleus' Skia context uses
 * that exact queue, so producer writes and the GPU snapshot performed by
 * [TextureView] are ordered without a CPU wait or a pixel readback.
 *
 * Both pointers are borrowed and valid only while this value remains current in
 * composition. A producer must stop using them from its `DisposableEffect`.
 */
public data class MacMetalTextureHost(
    /** Borrowed `id<MTLDevice>` pointer. */
    public val device: Long,
    /** Borrowed `id<MTLCommandQueue>` pointer used by the enclosing Skia context. */
    public val commandQueue: Long,
    /** Borrowed `NSView` pointer used to resolve the current NSScreen/EDR capability. */
    public val nativeView: Long,
)

/**
 * Returns the Metal host of the current macOS Tao composition surface.
 * Returns null outside a live Metal-backed macOS Tao surface.
 */
@Composable
public fun currentMacMetalTextureHost(): MacMetalTextureHost? {
    val host = LocalTaoMetalTextureHost.current ?: return null
    if (
        Platform.Current != Platform.MacOS ||
        host.metalDevicePtr == 0L ||
        host.metalCommandQueuePtr == 0L ||
        host.nativeViewPtr == 0L
    ) {
        return null
    }
    return remember(host) {
        MacMetalTextureHost(
            device = host.metalDevicePtr,
            commandQueue = host.metalCommandQueuePtr,
            nativeView = host.nativeViewPtr,
        )
    }
}

/**
 * macOS implementation of [TextureView]. The producer's `IOSurface` (or
 * `id<MTLTexture>`) is mapped as an `id<MTLTexture>` on the window's own Metal
 * device and wrapped in a Skia [Surface]; each producer frame is then pulled
 * into an immutable GPU [Image] with `makeImageSnapshot()` and composited into
 * the Compose scene.
 *
 * The snapshot is why macOS needs one GPU-GPU copy per frame: skiko exposes
 * `BackendRenderTarget.makeMetal` but no Metal `BackendTexture`, so Skia can
 * only *wrap* the import as a render target, and an image of a wrapped render
 * target is always a copy. The copy is recorded on the window's Skia context
 * and executed inside the same flush as the draw that samples it, so the
 * composited frame is always the one the producer had published when the frame
 * was drawn — the equivalent of the Windows keyed-mutex staging path.
 *
 * Threading follows the macOS record/replay split: the composable's draw pass
 * never waits for the render thread. A new producer stamp schedules its GPU
 * snapshot on the render thread and the current immutable image is used for
 * that draw; completion invalidates the draw pass once and publishes the fresh
 * image on the next frame. This one-frame pipeline is essential during native
 * live resize, where `nextDrawable()` may temporarily occupy the render thread
 * while AppKit still needs the main thread to deliver window geometry events.
 */
@Composable
internal fun MacTextureView(
    source: TextureViewSource,
    modifier: Modifier,
    controller: TextureViewController?,
    filterQuality: FilterQuality,
    contentScale: ContentScale,
    alignment: Alignment,
) {
    val host = LocalTaoMetalTextureHost.current
    if (Platform.Current != Platform.MacOS || host == null || !NativeTaoMacOsTextureBridge.isLoaded) {
        Box(modifier)
        return
    }

    val imported =
        remember(source, host) {
            TextureImportLease(metalTextureImports, host, source)
        }.imported
    if (imported == null) {
        Box(modifier)
        return
    }
    val snapshotInvalidation = remember(imported) { mutableLongStateOf(0L) }
    val invalidateSnapshot =
        remember(imported) {
            { snapshotInvalidation.longValue += 1L }
        }

    val srcRect =
        remember(imported) {
            Rect(0f, 0f, imported.widthPx.toFloat(), imported.heightPx.toFloat())
        }
    val sampling = remember(filterQuality) { samplingFor(filterQuality) }
    Box(
        modifier.drawBehind {
            // Read the completion stamp so an asynchronously prepared snapshot invalidates only
            // this draw pass. Compose MutableState supports producer-thread writes just like
            // TextureViewController.frameStamp does.
            snapshotInvalidation.longValue
            // Snapshot read of the frame stamp: markFrameAvailable()
            // invalidates exactly this draw pass, nothing recomposes.
            val stamp = controller?.frameStamp?.longValue ?: 0L
            val image =
                imported.snapshot(controller, stamp, invalidateSnapshot) ?: return@drawBehind
            drawExternalTexture(image, srcRect, contentScale, alignment, sampling)
        },
    )
}

/**
 * One imported external texture: the native `id<MTLTexture>` mapping plus the
 * Skia objects wrapping it, and the current frame's snapshot. Everything Skia
 * touches lives on [host]'s render thread. The current immutable image is
 * published through a volatile reference so the main-thread draw pass never
 * waits; creation and destruction remain serialized on the owning render
 * thread.
 */
private class MacImportedTexture(
    val handle: Long,
    val host: TaoMetalTextureHost,
    private val renderTarget: BackendRenderTarget,
    private val surface: Surface,
    val widthPx: Int,
    val heightPx: Int,
) {
    @Volatile
    private var image: Image? = null

    @Volatile
    private var closed: Boolean = false

    private val refreshInFlight = AtomicBoolean(false)
    private val retiredImages = ArrayDeque<Image>()
    private val pendingInvalidations = LinkedHashSet<() -> Unit>()
    private val pendingInvalidationsLock = Any()

    /** One snapshot per producer frame per controller — see [FrameStampGate]. */
    private val consumed = FrameStampGate()

    /**
     * Current GPU snapshot of the producer surface, re-pulled once per signalled
     * frame however many views share this import. The previous image is closed
     * only once the new one exists — pictures recorded from earlier frames keep
     * their own Skia reference, so an in-flight replay is unaffected, and a
     * failed snapshot leaves the last good frame on screen instead of blanking
     * the view.
     */
    fun snapshot(
        controller: TextureViewController?,
        stamp: Long,
        onSnapshotReady: () -> Unit,
    ): Image? {
        val current = image
        if (closed) return current
        if (current != null && !consumed.isPending(controller, stamp)) return current
        if (!consumed.isPending(controller, stamp)) return current
        synchronized(pendingInvalidationsLock) {
            if (!closed) pendingInvalidations += onSnapshotReady
        }
        // At most one worker may wait for this import's render thread. If a newer producer stamp
        // arrives meanwhile it remains pending; completion invalidates the draw pass and that
        // pass schedules the newest stamp next.
        if (!refreshInFlight.compareAndSet(false, true)) return current
        consumed.markConsumed(controller, stamp)
        textureSnapshotExecutor.execute {
            var installed = false
            var invalidations = emptyList<() -> Unit>()
            try {
                host.runOnRenderThread {
                    if (closed) return@runOnRenderThread
                    val previous = image
                    val fresh =
                        runCatching {
                            // The producer replaces the wrapped texture contents behind Skia's
                            // back. DISCARD invalidates Skia's cached snapshot without preserving
                            // the previous surface contents. RETAIN is wrong for a continuously
                            // overwritten video buffer: it triggers copy-on-write for every frame
                            // and leaves a stream of full-size IOSurfaces in the Metal cache.
                            surface.notifyContentWillChange(ContentChangeMode.DISCARD)
                            surface.makeImageSnapshot()
                        }.getOrNull()
                    if (fresh != null) {
                        // A picture recorded with the previous image can be replayed immediately
                        // after this task. Keep that image for one further snapshot generation;
                        // executor ordering then proves its replay has completed before close.
                        retiredImages.pollFirst()?.close()
                        if (previous != null) retiredImages.addLast(previous)
                        image = fresh
                        installed = true
                    }
                }
            } finally {
                invalidations =
                    synchronized(pendingInvalidationsLock) {
                        pendingInvalidations.toList().also { pendingInvalidations.clear() }
                    }
                refreshInFlight.set(false)
            }
            if (installed && !closed) invalidations.forEach { invalidate -> runCatching(invalidate) }
        }
        return image
    }

    fun close() {
        closed = true
        synchronized(pendingInvalidationsLock) { pendingInvalidations.clear() }
        // Skia teardown must happen on the context's thread, before the native
        // texture goes. The hop only fails once the render thread is gone — i.e.
        // after the host closed its DirectContext, which already freed these
        // objects — so swallowing that is safe, but the native import (and the
        // IOSurface reference it holds) must be released either way.
        runCatching {
            host.runOnRenderThread {
                image?.close()
                while (retiredImages.isNotEmpty()) retiredImages.removeFirst().close()
                surface.close()
                renderTarget.close()
            }
        }
        image = null
        consumed.clear()
        NativeTaoMacOsTextureBridge.nativeDestroy(handle)
    }
}

/**
 * Waiters are deliberately separate from every Metal owner thread: they may block on a surface's
 * render executor, but never AppKit's event thread. Daemon workers carry no application lifetime.
 */
private val textureSnapshotExecutor =
    Executors.newCachedThreadPool { task ->
        Thread(task, "TaoTextureSnapshot").apply { isDaemon = true }
    }

/**
 * The macOS import ledger. Refcounted and keyed by Skia context + source; see
 * [TextureImportRegistry] for why, and for the scaffolding the three backends
 * share. Nothing calls `closeAllFor` here: a macOS surface closes its
 * `DirectContext` only after the composition that leased through it is gone.
 */
private val metalTextureImports =
    TextureImportRegistry<TaoMetalTextureHost, MacImportedTexture>(
        contextOf = { it.directContext },
        importTexture = ::importTexture,
        closeImport = { it.close() },
    )

private fun importTexture(
    host: TaoMetalTextureHost,
    source: TextureViewSource,
): MacImportedTexture? {
    val widthPx: Int
    val heightPx: Int
    when (source) {
        is IOSurfaceTextureSource -> {
            widthPx = source.widthPx
            heightPx = source.heightPx
        }
        is MetalTextureSource -> {
            widthPx = source.widthPx
            heightPx = source.heightPx
        }
        else -> return null
    }
    if (widthPx < 1 || heightPx < 1 || host.metalDevicePtr == 0L) return null

    // The whole import runs on the render thread: the texture must be created
    // on the device Skia renders with, and the Skia wrappers are context-bound.
    return host.runOnRenderThread {
        val handle =
            when (source) {
                is IOSurfaceTextureSource ->
                    NativeTaoMacOsTextureBridge.nativeImportIOSurface(
                        host.metalDevicePtr,
                        source.ioSurface,
                        widthPx,
                        heightPx,
                    )
                is MetalTextureSource ->
                    NativeTaoMacOsTextureBridge.nativeImportMetalTexture(
                        host.metalDevicePtr,
                        source.metalTexture,
                        widthPx,
                        heightPx,
                    )
            }
        if (handle <= 0L) return@runOnRenderThread null
        val texturePtr = NativeTaoMacOsTextureBridge.nativeTexturePtr(handle)
        if (texturePtr == 0L) {
            NativeTaoMacOsTextureBridge.nativeDestroy(handle)
            return@runOnRenderThread null
        }
        val pixelFormat = NativeTaoMacOsTextureBridge.nativePixelFormat(handle)
        val colorFormat =
            when (pixelFormat) {
                NativeTaoMacOsTextureBridge.FORMAT_RGBA8 -> SurfaceColorFormat.RGBA_8888
                NativeTaoMacOsTextureBridge.FORMAT_RGBA16_FLOAT -> skikoRgbaF16SurfaceColorFormat
                else -> SurfaceColorFormat.BGRA_8888
            }
        val colorSpace =
            if (pixelFormat == NativeTaoMacOsTextureBridge.FORMAT_RGBA16_FLOAT) {
                ColorSpace.sRGBLinear
            } else {
                ColorSpace.sRGB
            }
        val renderTarget = BackendRenderTarget.makeMetal(widthPx, heightPx, texturePtr)
        val surface =
            runCatching {
                Surface.makeFromBackendRenderTarget(
                    context = host.directContext,
                    rt = renderTarget,
                    origin = SurfaceOrigin.TOP_LEFT,
                    colorFormat = colorFormat,
                    colorSpace = colorSpace,
                )
            }.getOrNull()
        if (surface == null) {
            renderTarget.close()
            NativeTaoMacOsTextureBridge.nativeDestroy(handle)
            return@runOnRenderThread null
        }
        MacImportedTexture(handle, host, renderTarget, surface, widthPx, heightPx)
    }
}
