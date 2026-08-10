package dev.nucleusframework.window.tao.scene

import androidx.compose.runtime.ProvidableCompositionLocal
import androidx.compose.runtime.State
import androidx.compose.runtime.compositionLocalOf
import dev.nucleusframework.window.tao.TextureViewController
import dev.nucleusframework.window.tao.TextureViewHostCapabilities
import dev.nucleusframework.window.tao.UnavailableTextureViewHostCapabilitiesState
import dev.nucleusframework.window.tao.ffi.NativeTaoEglBridge
import dev.nucleusframework.window.tao.ffi.NativeTaoLinuxTextureBridge
import org.jetbrains.skia.DirectContext

/**
 * Linux: the EGL/Skia surface the enclosing Compose scene draws into. Consumed
 * by the `TextureView` composable, which must create its GL texture in the very
 * EGL context — and adopt it into the very Skia context — that will paint the
 * scene; a GPU image belongs to exactly one `DirectContext`.
 *
 * Like macOS (and unlike Windows' single shared ANGLE context), every Linux
 * surface owns a private EGL context and its own `DirectContext`, so each one
 * provides its own [LocalTaoGlTextureHost]: [TaoComposeSceneHostLinux] for the
 * window scene, [dev.nucleusframework.window.tao.popup.TaoPopupSceneLayerLinux]
 * for native popup layers,
 * [dev.nucleusframework.window.tao.popup.TaoStandalonePopupHostLinux] for tray
 * panels.
 *
 * Threading: everything here runs on the Tao event-loop thread. The context is
 * already current during composition and the draw pass (both happen inside
 * `ComposeScene.render()`, between the host's make-current/release), which is
 * exactly when imports are created and drawn; [withContextCurrent] binds it for
 * the teardown paths that run outside a render pass.
 */
internal interface TaoGlTextureHost {
    val textureViewHostCapabilities: State<TextureViewHostCapabilities>
        get() = UnavailableTextureViewHostCapabilitiesState

    /** Skia context of this surface. */
    val directContext: DirectContext

    /** Records a texture frame sampled by the current scene render. */
    fun markTextureFrameSampled(controller: TextureViewController) {
        LinuxTextureReleaseFenceRegistry.mark(this, controller)
    }

    /**
     * Publishes release fences after Skia submitted every sampling command for
     * the frame. Must run with this host's EGL context current.
     */
    fun publishTextureReleaseFences() {
        LinuxTextureReleaseFenceRegistry.publish(this)
    }

    /**
     * Runs [block] with this surface's EGL context current, and returns its
     * result — or null when the context could not be bound (the surface is
     * being torn down, or another thread holds it). Skia GL deletes must never
     * run without a current context: the entry points Skia resolved through
     * `eglGetProcAddress` dereference thread-local driver state and crash.
     */
    fun <T> withContextCurrent(block: () -> T): T?
}

/**
 * Pending sampled frames are kept outside anonymous host implementations so
 * window, popup and standalone surfaces all share the exact same lifecycle.
 * Weak host keys prevent an abandoned surface from pinning its GPU context.
 */
private object LinuxTextureReleaseFenceRegistry {
    private val pending =
        java.util
            .WeakHashMap<TaoGlTextureHost, java.util.IdentityHashMap<TextureViewController, Unit>>()

    fun mark(
        host: TaoGlTextureHost,
        controller: TextureViewController,
    ) {
        synchronized(pending) {
            pending.getOrPut(host) { java.util.IdentityHashMap() }[controller] = Unit
        }
    }

    fun publish(host: TaoGlTextureHost) {
        val controllers =
            synchronized(pending) {
                pending
                    .remove(host)
                    ?.keys
                    ?.toList()
                    .orEmpty()
            }
        controllers.forEach { controller ->
            controller.replaceReleaseFence(NativeTaoLinuxTextureBridge.nativeCreateReleaseFence())
        }
    }
}

/**
 * Shared [TaoGlTextureHost.withContextCurrent] implementation. Fast path: the
 * context is already current (composition, draw pass, popup render) and the
 * block runs inline.
 *
 * The slow path binds — and then **restores whatever was current before**, not
 * merely unbinds. A disposal path for one surface can run inside another
 * surface's render pass: a popup's `TextureView` leaves the composition while
 * the parent window's scene is mid-render, with the window's context current.
 * Unbinding there would leave the thread with no context, and the rest of the
 * host frame (frame decoration, `flushAndSubmit`) would issue GL against
 * nothing — the Linux twin of the ANGLE surface-restore bug this feature fixed
 * on Windows.
 *
 * [attachment] must be read live from the owning surface: it is 0 (and this
 * returns null) once the surface detached, which is what keeps a late disposal
 * from dereferencing a freed `EglAttachment`.
 */
internal fun <T> withEglContextCurrent(
    attachment: Long,
    block: () -> T,
): T? {
    if (attachment == 0L || !LinuxEglBindingSnapshot.isAvailable) return null
    if (NativeTaoLinuxTextureBridge.nativeIsAttachmentCurrent(attachment)) return block()
    // Refused when a snapshot is already outstanding on this thread: rebinding
    // would then lose the outer binding, so give up instead (the caller treats
    // null as "context unavailable" and skips its GL work).
    if (!LinuxEglBindingSnapshot.save()) return null
    NativeTaoEglBridge.nativeMakeCurrent(attachment)
    if (!NativeTaoLinuxTextureBridge.nativeIsAttachmentCurrent(attachment)) {
        restoreDisplacedBinding(attachment)
        return null
    }
    return try {
        block()
    } finally {
        restoreDisplacedBinding(attachment)
    }
}

/** Puts back the binding [withEglContextCurrent] displaced, or unbinds if there was none. */
private fun restoreDisplacedBinding(attachment: Long) {
    if (!LinuxEglBindingSnapshot.restore()) {
        NativeTaoEglBridge.nativeReleaseCurrent(attachment)
    }
}

/**
 * Runs [block] and puts back the EGL binding it displaced — the counterpart of
 * [withEglContextCurrent] for code that *creates* (or renders into) a surface of
 * its own: `nativeAttach*` leaves the fresh context current, and Skia's
 * `DirectContext` bring-up needs it, but that work can run inside **another**
 * surface's render pass.
 *
 * A `TaoStandalonePopup` composed into a live window builds its host from
 * `remember {}`, i.e. inside `ComposeScene.render()` with the window's context
 * current. Without this, the rest of the window frame (frame decoration,
 * `flushAndSubmit`) would issue GL against the panel's 1x1 context and
 * desynchronise the window's Skia state cache for good — a permanently garbled
 * window, not just one lost frame.
 *
 * When nothing was current beforehand, the newly bound context simply stays
 * current: nothing was displaced, so there is nothing to put back.
 *
 * See [preservingBinding] for the save/restore contract, and
 * [preservingAngleBinding] for the Windows counterpart.
 */
internal fun <T> preservingEglBinding(block: () -> T): T = preservingBinding(LinuxEglBindingSnapshot, block)

internal val LocalTaoGlTextureHost: ProvidableCompositionLocal<TaoGlTextureHost?> =
    compositionLocalOf { null }
