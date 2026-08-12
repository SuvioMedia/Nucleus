package dev.nucleusframework.window.tao

import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import androidx.compose.runtime.mutableStateOf
import dev.nucleusframework.core.runtime.Platform
import dev.nucleusframework.window.WindowDynamicRangeMode
import dev.nucleusframework.window.tao.scene.LocalTaoGlTextureHost
import dev.nucleusframework.window.tao.scene.LocalTaoMetalTextureHost
import dev.nucleusframework.window.tao.scene.LocalTaoWindowsTextureHost
import java.util.concurrent.atomic.AtomicLong

/** Actual output range of the composition surface hosting a [TextureView]. */
public enum class TextureViewHostDynamicRange {
    SDR,
    HDR,
}

/** Readiness of the current host generation. */
public enum class TextureViewHostPresentationState {
    /** The device/output is being configured and has not presented yet. */
    PENDING,

    /** At least one frame was presented by this exact host generation. */
    PRESENTED,

    /** No compatible GPU composition host is available. */
    UNAVAILABLE,
}

/** Native pixel format used by the composition surface that reaches the system compositor. */
@Suppress("MagicNumber")
public enum class TextureViewHostPixelFormat(
    public val componentBitDepth: Int,
) {
    /** No live composition surface, or its format could not be queried. */
    UNKNOWN(0),

    /** Conventional 8-bit sRGB output. */
    RGBA8_SRGB(8),

    /** Half-float, linear scRGB output. Values outside `[0, 1]` remain representable. */
    RGBA16_FLOAT_SCRGB(16),

    /** Packed 10-bit BT.2020/PQ output used when a compositor does not expose scRGB. */
    RGB10_A2_BT2020_PQ(10),
}

/** Platform handle information a texture producer needs for zero-copy interop. */
public sealed interface TextureViewProducerInfo

/** Windows producer target. [adapterLuid] uses the native DXGI LUID bit layout. */
public data class WindowsTextureViewProducerInfo(
    public val adapterLuid: Long,
) : TextureViewProducerInfo

/** Borrowed Metal objects of the active macOS composition surface. */
public data class MacTextureViewProducerInfo(
    public val device: Long,
    public val commandQueue: Long,
) : TextureViewProducerInfo

/** One DRM format and the modifiers the active EGL device can import. */
public data class LinuxTextureFormatModifiers(
    public val format: Int,
    public val modifiers: List<Long>,
)

/** Linux producer target resolved from the active EGL/DRM device. */
public data class LinuxTextureViewProducerInfo(
    public val renderNode: String?,
    public val formats: List<LinuxTextureFormatModifiers>,
    public val supportsAcquireFences: Boolean,
    public val supportsReleaseFences: Boolean,
) : TextureViewProducerInfo

/**
 * Observable capabilities of the exact surface currently hosting composition.
 *
 * [generation] changes whenever the active monitor, GPU device, output colour
 * configuration or native surface changes. [presentedFrameCount] is `0` until
 * the first real system present for that generation and `1` afterwards. It is
 * deliberately a saturating marker, rather than a frame sequence, so observing
 * capabilities does not invalidate composition on every presented frame.
 */
public data class TextureViewHostCapabilities(
    public val requestedMode: WindowDynamicRangeMode,
    public val actualDynamicRange: TextureViewHostDynamicRange,
    public val presentationState: TextureViewHostPresentationState,
    public val sdrWhiteLevelNits: Float?,
    public val maximumLuminanceNits: Float?,
    public val headroom: Float,
    public val generation: Long,
    public val presentedFrameCount: Long,
    /** Exact pixel format handed to the platform compositor for this host generation. */
    public val outputPixelFormat: TextureViewHostPixelFormat = TextureViewHostPixelFormat.UNKNOWN,
    public val producerInfo: TextureViewProducerInfo?,
) {
    init {
        require(sdrWhiteLevelNits == null || (sdrWhiteLevelNits.isFinite() && sdrWhiteLevelNits > 0f))
        require(maximumLuminanceNits == null || (maximumLuminanceNits.isFinite() && maximumLuminanceNits > 0f))
        require(headroom.isFinite() && headroom >= 1f)
        require(generation >= 0L)
        require(presentedFrameCount >= 0L)
    }

    /** Host capability states used when no renderer is available. */
    public companion object {
        /** No live Tao GPU host in the current composition. */
        public val UNAVAILABLE: TextureViewHostCapabilities =
            TextureViewHostCapabilities(
                requestedMode = WindowDynamicRangeMode.STANDARD,
                actualDynamicRange = TextureViewHostDynamicRange.SDR,
                presentationState = TextureViewHostPresentationState.UNAVAILABLE,
                sdrWhiteLevelNits = null,
                maximumLuminanceNits = null,
                headroom = 1f,
                generation = 0L,
                presentedFrameCount = 0L,
                outputPixelFormat = TextureViewHostPixelFormat.UNKNOWN,
                producerInfo = null,
            )
    }
}

internal val UnavailableTextureViewHostCapabilitiesState: State<TextureViewHostCapabilities> =
    mutableStateOf(TextureViewHostCapabilities.UNAVAILABLE)

private val textureViewHostGenerationSequence = AtomicLong()

/**
 * Turns backend-local surface generations into process-unique public ones.
 *
 * Native backends restart their counters when a swapchain/context is rebuilt.
 * The public contract cannot: a producer may otherwise mistake a fresh set of
 * borrowed device handles for the previous surface when both report `1`.
 */
internal class TextureViewHostGenerationTracker {
    private var surfaceToken: Long? = null
    private var nativeGeneration: Long? = null
    private var exposedGeneration: Long = 0L

    fun resolve(
        surfaceToken: Long,
        nativeGeneration: Long,
    ): Long {
        if (this.surfaceToken != surfaceToken || this.nativeGeneration != nativeGeneration) {
            this.surfaceToken = surfaceToken
            this.nativeGeneration = nativeGeneration
            exposedGeneration = textureViewHostGenerationSequence.incrementAndGet()
        }
        return exposedGeneration
    }

    fun reset() {
        surfaceToken = null
        nativeGeneration = null
        exposedGeneration = 0L
    }
}

/** A presentation-readiness marker that stays snapshot-stable after the first present. */
internal fun textureViewPresentedFrameMarker(nativePresentedFrameCount: Long): Long =
    if (nativePresentedFrameCount > 0L) 1L else 0L

/** Returns live capabilities of the Tao surface containing this composition. */
@Composable
public fun currentTextureViewHostCapabilities(): TextureViewHostCapabilities =
    when (Platform.Current) {
        Platform.Windows ->
            LocalTaoWindowsTextureHost.current
                ?.textureViewHostCapabilities
                ?.value
                ?: TextureViewHostCapabilities.UNAVAILABLE

        Platform.MacOS ->
            LocalTaoMetalTextureHost.current
                ?.textureViewHostCapabilities
                ?.value
                ?: TextureViewHostCapabilities.UNAVAILABLE

        Platform.Linux ->
            LocalTaoGlTextureHost.current
                ?.textureViewHostCapabilities
                ?.value
                ?: TextureViewHostCapabilities.UNAVAILABLE

        else -> TextureViewHostCapabilities.UNAVAILABLE
    }
