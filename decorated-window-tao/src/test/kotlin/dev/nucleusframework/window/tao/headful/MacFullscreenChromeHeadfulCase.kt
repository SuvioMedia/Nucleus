package dev.nucleusframework.window.tao.headful

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import dev.nucleusframework.window.TitleBar
import dev.nucleusframework.window.TitleBarPlacement
import dev.nucleusframework.window.WindowScaffold
import dev.nucleusframework.window.newFullscreenControls
import dev.nucleusframework.window.tao.ffi.NativeMetalBridge
import java.awt.Point
import java.awt.Rectangle
import java.awt.image.BufferedImage
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean
import javax.imageio.ImageIO
import kotlin.math.abs
import kotlin.math.roundToInt
import java.awt.Color as AwtColor

/** Click the controls that are actually painted, not coordinates from a native probe. */
internal object MacFullscreenChromeHeadfulCase {
    fun create(isMac: Boolean): TaoWindowTestCase =
        TaoWindowTestCase(
            name = "macOS fullscreen menu and title bar reveal as one clickable unit",
            timeoutMillis = 60_000L,
            paintDefaultBackground = false,
            skip = {
                when {
                    !isMac -> "macOS only"
                    !NativeMetalBridge.isLoaded -> "macOS native bridge unavailable"
                    !NativeMetalBridge.nativeIsMacOSTahoeOrLater() -> "requires macOS 26 or later"
                    else -> null
                }
            },
            content = {
                val scope = this
                WindowScaffold(
                    titleBar = {
                        with(scope) {
                            TitleBar(Modifier.newFullscreenControls()) {
                                // This marks a second Compose title bar, if it erroneously
                                // gets painted underneath AppKit during the reveal.
                                Box(Modifier.align(Alignment.Start).size(24.dp).background(Color.Magenta))
                            }
                        }
                    },
                    titleBarPlacement = TitleBarPlacement.Overlay(autoHideInFullscreen = false),
                ) {
                    Box(Modifier.fillMaxSize().background(Color(0xFF203040)))
                }
            },
            driver = {
                verifyChrome(enterViaButton = false)
                verifyChrome(enterViaButton = true)
            },
        )

    private suspend fun TaoWindowTestScope.verifyChrome(enterViaButton: Boolean) {
        awaitUntil("window mapped") { bounds() != null }
        window.focus()
        settle()
        val scale = window.scaleFactor.coerceAtLeast(1f)
        val windowed = requireNotNull(bounds())
        val origin = Point((windowed[0] / scale).roundToInt(), (windowed[1] / scale).roundToInt())
        move(origin.x + 300, origin.y + 300)
        if (enterViaButton) {
            val zoom = awaitControl(origin, TrafficLight.Green)
            move(origin.x + zoom.x, origin.y + zoom.y)
            click()
        } else {
            window.setFullscreen(true)
        }
        awaitUntil("fullscreen size") { (bounds()?.get(2) ?: 0) > windowed[2] + 200 }
        settle(1_000)
        val fullscreen = requireNotNull(bounds())
        val screen = Point((fullscreen[0] / scale).roundToInt(), (fullscreen[1] / scale).roundToInt())
        val middleX = screen.x + (fullscreen[2] / scale / 2).roundToInt()
        move(middleX, screen.y + 300)
        click()
        settle(1_000)
        check(findControl(capture(screen), TrafficLight.Green) == null) {
            "fullscreen controls remained visible at rest"
        }

        for (y in 300 downTo 0 step 2) {
            move(screen.x + 200, screen.y + y)
            if (y <= CAPTURE_HEIGHT) assertSingleTitleBar(capture(screen))
        }
        repeat(REVEAL_FRAME_COUNT) {
            val frame = capture(screen)
            assertSingleTitleBar(frame)
            settle(REVEAL_FRAME_DELAY_MS)
        }
        val zoom = awaitControl(screen, TrafficLight.Green)
        // Cross the menu/toolbar boundary slowly, then travel along the
        // toolbar. A teleport can miss the real rollover failure entirely.
        for (y in 2..zoom.y step 2) {
            move(middleX, screen.y + y)
            settle(15)
        }
        for (step in 1..ROUTE_STEPS) {
            val x = middleX + (screen.x + zoom.x - middleX) * step / ROUTE_STEPS
            move(x, screen.y + zoom.y)
            settle(ROUTE_STEP_MS)
            check(findControl(capture(screen), TrafficLight.Green) != null) {
                "fullscreen controls retracted on the pointer route at step $step"
            }
        }
        click()
        awaitUntil("green click left fullscreen") {
            abs((bounds()?.get(2) ?: 0) - windowed[2]) <= 64
        }
        settle(700)

        val restored = requireNotNull(bounds())
        val restoredOrigin = Point((restored[0] / scale).roundToInt(), (restored[1] / scale).roundToInt())
        val mini = awaitControl(restoredOrigin, TrafficLight.Yellow)
        move(restoredOrigin.x + mini.x, restoredOrigin.y + mini.y)
        click()
        awaitUntil("yellow click minimized window") { window.isMinimized }
        window.setMinimized(false)
        window.focus()
        awaitUntil("window restored after minimize") { !window.isMinimized }
        settle()

        val closed = AtomicBoolean(false)
        window.onCloseRequested { closed.set(true) }
        val close = awaitControl(restoredOrigin, TrafficLight.Red)
        move(restoredOrigin.x + close.x, restoredOrigin.y + close.y)
        click()
        awaitUntil("red click requested close") { closed.get() }
    }

    private enum class TrafficLight { Red, Yellow, Green }

    private suspend fun TaoWindowTestScope.awaitControl(
        origin: Point,
        control: TrafficLight,
    ): Point {
        repeat(CONTROL_ATTEMPTS) {
            findControl(capture(origin), control)?.let { return it }
            settle(100)
        }
        val report = File("build/reports/tao-headful/fullscreen-chrome-missing.png")
        report.parentFile.mkdirs()
        ImageIO.write(capture(origin), "png", report)
        error("visible $control control absent")
    }

    private suspend fun capture(origin: Point): BufferedImage =
        requireNotNull(
            HeadfulRobot.inject { robot ->
                robot.createScreenCapture(Rectangle(origin.x, origin.y, CAPTURE_WIDTH, CAPTURE_HEIGHT))
            },
        ) { "screen capture unavailable: ${HeadfulRobot.unavailableReason}" }

    private fun findControl(
        image: BufferedImage,
        control: TrafficLight,
    ): Point? {
        var count = 0
        var sumX = 0
        var sumY = 0
        for (y in MENU_BAR_MIN_HEIGHT until image.height) {
            for (x in 0 until CONTROL_AREA_WIDTH) {
                val color = AwtColor(image.getRGB(x, y))
                val matches =
                    when (control) {
                        TrafficLight.Red ->
                            color.red > 190 && color.red - color.green > 60 && color.red - color.blue > 40
                        TrafficLight.Yellow -> color.red > 190 && color.green > 150 && color.blue < 100
                        TrafficLight.Green ->
                            color.green > 140 && color.green - color.red > 55 && color.green - color.blue > 55
                    }
                if (matches) {
                    count++
                    sumX += x
                    sumY += y
                }
            }
        }
        return if (count >= MIN_CONTROL_PIXELS) Point(sumX / count, sumY / count) else null
    }

    private fun containsComposeMarker(image: BufferedImage): Boolean {
        var count = 0
        for (y in 0 until image.height) {
            for (x in 0 until image.width) {
                val color = AwtColor(image.getRGB(x, y))
                if (color.red > 200 && color.blue > 200 && color.green < 80) count++
            }
        }
        return count > MIN_CONTROL_PIXELS
    }

    private fun assertSingleTitleBar(frame: BufferedImage) {
        check(!containsComposeMarker(frame)) { "a second Compose title bar appeared during native reveal" }
    }

    private suspend fun TaoWindowTestScope.move(
        x: Int,
        y: Int,
    ) {
        check(NativeMetalBridge.nativeDiagMovePointer(x, y)) { "native pointer injection unavailable" }
        settle(30)
    }

    private suspend fun TaoWindowTestScope.click() {
        check(NativeMetalBridge.nativeDiagClickPointer()) { "native click injection unavailable" }
        settle(30)
    }

    private const val CAPTURE_WIDTH = 640
    private const val CAPTURE_HEIGHT = 140
    private const val CONTROL_AREA_WIDTH = 120
    private const val MENU_BAR_MIN_HEIGHT = 8
    private const val MIN_CONTROL_PIXELS = 12
    private const val CONTROL_ATTEMPTS = 40
    private const val REVEAL_FRAME_COUNT = 24
    private const val REVEAL_FRAME_DELAY_MS = 30L
    private const val ROUTE_STEPS = 12
    private const val ROUTE_STEP_MS = 200L
}
