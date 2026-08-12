package dev.nucleusframework.window

/**
 * Requested dynamic-range mode of a desktop window.
 *
 * [EXTENDED_IF_AVAILABLE] keeps the whole Compose scene in an extended-linear
 * floating-point working space and asks the platform compositor for an HDR/EDR
 * output. The request is best-effort: callers must inspect the active
 * TextureView host capabilities before treating the output as HDR.
 */
public enum class WindowDynamicRangeMode {
    /** Use the platform's standard SDR presentation path. */
    STANDARD,

    /** Use an extended-linear scene and HDR/EDR output when the active display supports it. */
    EXTENDED_IF_AVAILABLE,
}
