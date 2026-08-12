package dev.nucleusframework.window.tao

import dev.nucleusframework.window.tao.dispatch.TaoMainDispatcher
import dev.nucleusframework.window.tao.ffi.NativeTaoWindowsDecoBridge
import java.util.concurrent.CopyOnWriteArrayList

internal class TaoFullscreenListeners {
    private val prepareListeners = CopyOnWriteArrayList<(Int, Int, Boolean) -> Unit>()
    private val transitionListeners = CopyOnWriteArrayList<(Boolean, Boolean) -> Unit>()

    fun onPrepare(block: (width: Int, height: Int, fullscreen: Boolean) -> Unit) {
        prepareListeners += block
    }

    fun dispatchPrepare(
        width: Int,
        height: Int,
        fullscreen: Boolean,
    ) {
        prepareListeners.forEach { it(width, height, fullscreen) }
    }

    fun onTransition(block: (fullscreen: Boolean, completed: Boolean) -> Unit) {
        transitionListeners += block
    }

    fun dispatchTransition(
        fullscreen: Boolean,
        completed: Boolean,
    ) {
        transitionListeners.forEach { it(fullscreen, completed) }
    }

    fun setWindowsFullscreen(
        hwnd: Long,
        fullscreen: Boolean,
    ) {
        // Pre-layout is main-thread only: the prepare hook renders on this
        // stack, and the EGL context lives on the Tao loop thread. An
        // off-thread caller still gets a correct (if less atomic) toggle
        // via the async RESIZED event.
        if (Thread.currentThread() === TaoMainDispatcher.taoMainThread) {
            val targetSize = NativeTaoWindowsDecoBridge.nativeGetFullscreenTargetSize(hwnd, fullscreen)
            if (targetSize != null && targetSize.size == 2 && targetSize.all { it > 0 }) {
                dispatchPrepare(targetSize[0], targetSize[1], fullscreen)
            }
        }
        NativeTaoWindowsDecoBridge.nativeSetFullscreen(hwnd, fullscreen)
    }
}
