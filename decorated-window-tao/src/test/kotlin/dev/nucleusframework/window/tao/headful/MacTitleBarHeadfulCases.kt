package dev.nucleusframework.window.tao.headful

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.PointerEventType
import androidx.compose.ui.input.pointer.onPointerEvent
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import dev.nucleusframework.window.TitleBar
import dev.nucleusframework.window.WindowScaffold
import dev.nucleusframework.window.tao.TaoEventCode
import dev.nucleusframework.window.tao.TaoMouseButton
import dev.nucleusframework.window.tao.ffi.NativeMetalBridge
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** macOS title-bar input probes that do not require global Accessibility permission. */
internal object MacTitleBarHeadfulCases {
    private const val POINTER_FIXED_SCALE = 1024f
    private const val INPUT_DISPATCH_SETTLE_MILLIS = 25L

    private val isMac: Boolean =
        System.getProperty("os.name", "").lowercase().let {
            it.contains("mac") || it.contains("darwin")
        }

    /** A normal macOS title-bar double-click must zoom and a second one must restore. */
    @OptIn(ExperimentalComposeUiApi::class)
    fun doubleClickZoomsAndRestores(): TaoWindowTestCase {
        val titleBarPresses = AtomicInteger(0)
        val titleBarBounds = AtomicReference<Rect?>(null)
        return TaoWindowTestCase(
            name = "macOS title bar double-click zooms and restores",
            paintDefaultBackground = false,
            skip = {
                when {
                    !isMac -> "macOS-only title-bar behavior"
                    java.awt.GraphicsEnvironment.isHeadless() -> "no display for window probe"
                    else -> null
                }
            },
            content = {
                WindowScaffold(
                    titleBar = {
                        TitleBar(
                            modifier =
                                Modifier
                                    .onGloballyPositioned { titleBarBounds.set(it.boundsInRoot()) }
                                    .onPointerEvent(PointerEventType.Press) {
                                        titleBarPresses.incrementAndGet()
                                    },
                        )
                    },
                ) {
                    Box(Modifier.fillMaxSize().background(Color(0xFF203040)))
                }
            },
            driver = {
                awaitUntil("window mapped") { bounds() != null }
                window.focus()
                settle(500)
                check(!window.isMaximized) { "test window unexpectedly starts maximized" }
                check(NativeMetalBridge.isLoaded) { "macOS Metal bridge is unavailable" }

                awaitUntil("title bar laid out") { titleBarBounds.get() != null }
                val bar = requireNotNull(titleBarBounds.get())
                val xFixed = (bar.center.x * POINTER_FIXED_SCALE).toInt()
                val yFixed = (bar.center.y * POINTER_FIXED_SCALE).toInt()
                val clickGapMillis =
                    (
                        // Leave enough headroom for the slower GraalVM native-image
                        // event pump. The injected presses still exercise the real
                        // system-configured double-click path, but scheduling jitter
                        // must not push the second press past AppKit's deadline.
                        NativeMetalBridge.nativeDoubleClickIntervalMillis() / 4L -
                            INPUT_DISPATCH_SETTLE_MILLIS
                    ).coerceAtLeast(1L)

                suspend fun doubleClick() {
                    var expectedPresses = titleBarPresses.get()
                    window.dispatch(TaoEventCode.CURSOR_MOVED, xFixed, yFixed)
                    settle(INPUT_DISPATCH_SETTLE_MILLIS)

                    window.dispatch(TaoEventCode.MOUSE_DOWN, TaoMouseButton.LEFT, 0)
                    expectedPresses++
                    awaitUntil("first injected press reached title bar") {
                        titleBarPresses.get() == expectedPresses
                    }
                    window.dispatch(TaoEventCode.MOUSE_UP, TaoMouseButton.LEFT, 0)
                    settle(INPUT_DISPATCH_SETTLE_MILLIS + clickGapMillis)

                    window.dispatch(TaoEventCode.MOUSE_DOWN, TaoMouseButton.LEFT, 0)
                    expectedPresses++
                    awaitUntil("second injected press reached title bar") {
                        titleBarPresses.get() == expectedPresses
                    }
                    window.dispatch(TaoEventCode.MOUSE_UP, TaoMouseButton.LEFT, 0)
                    settle(INPUT_DISPATCH_SETTLE_MILLIS)
                }

                doubleClick()
                awaitUntil("title-bar double-click maximize") { window.isMaximized }
                settle(700)
                doubleClick()
                awaitUntil("title-bar double-click restore") { !window.isMaximized }
            },
        )
    }
}
