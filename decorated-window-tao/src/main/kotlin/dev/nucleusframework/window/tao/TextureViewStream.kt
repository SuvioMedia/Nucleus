package dev.nucleusframework.window.tao

import androidx.compose.foundation.layout.Box
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.RememberObserver
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.layout.ContentScale
import java.util.IdentityHashMap
import java.util.concurrent.atomic.AtomicBoolean

private const val NO_FENCE: Int = TextureViewController.NO_FENCE

/**
 * One producer-owned buffer submitted to a [TextureViewStreamController].
 *
 * Ownership transfers to the controller on [TextureViewStreamController.submitFrame].
 * [onReleased] is invoked exactly once, including when this frame is superseded
 * before Compose ever sees it. A non-negative [acquireFenceFd] is a Linux
 * `sync_file`; Nucleus owns it after submission. [onReleased] owns a non-negative
 * release fence passed to it. The current backends return [NO_FENCE] when they
 * complete consumption synchronously or through an internal GPU copy.
 */
public class TextureViewFrame(
    public val source: TextureViewSource,
    public val acquireFenceFd: Int = NO_FENCE,
    public val onReleased: (releaseFenceFd: Int) -> Unit = {},
) {
    private val submitted = AtomicBoolean(false)
    private val released = AtomicBoolean(false)
    private val fenceTransferred = AtomicBoolean(false)

    internal val isReleased: Boolean get() = released.get()

    internal fun claimForSubmission() {
        check(submitted.compareAndSet(false, true)) { "A TextureViewFrame may be submitted only once" }
    }

    internal fun takeAcquireFence(): Int {
        if (acquireFenceFd == NO_FENCE) return NO_FENCE
        return if (fenceTransferred.compareAndSet(false, true)) acquireFenceFd else NO_FENCE
    }

    internal fun release(releaseFenceFd: Int = NO_FENCE) {
        if (!released.compareAndSet(false, true)) {
            if (releaseFenceFd != NO_FENCE) closeAcquireFenceFd(releaseFenceFd)
            return
        }
        if (acquireFenceFd != NO_FENCE && !fenceTransferred.get()) {
            closeAcquireFenceFd(acquireFenceFd)
        }
        runCatching { onReleased(releaseFenceFd) }
            .onFailure {
                if (releaseFenceFd != NO_FENCE) closeAcquireFenceFd(releaseFenceFd)
                textureLogger.warning { "TextureView frame release callback failed: ${it.message}" }
            }
    }

    public companion object {
        /** No acquire/release fence. */
        public const val NO_FENCE: Int = TextureViewController.NO_FENCE
    }
}

/**
 * Single-consumer queue for rotating video buffers.
 *
 * Submitting never blocks the producer. Only the newest not-yet-consumed frame
 * is retained; skipped frames are released immediately. A frame already owned
 * by the composable remains alive until that composition stops referring to it.
 */
public class TextureViewStreamController : AutoCloseable {
    internal val currentFrame = mutableStateOf<TextureViewFrame?>(null)

    private val lock = Any()
    private val acquiredFrames = IdentityHashMap<TextureViewFrame, Int>()
    private val pendingReleaseFences = IdentityHashMap<TextureViewFrame, Int>()
    private var consumerToken: Any? = null
    private var closed = false

    /** Transfers ownership of [frame] to this stream. Safe from any thread. */
    public fun submitFrame(frame: TextureViewFrame) {
        var skipped: TextureViewFrame? = null
        var skippedReleaseFence = NO_FENCE
        synchronized(lock) {
            check(!closed) { "TextureViewStreamController is closed" }
            frame.claimForSubmission()
            val previous = currentFrame.value
            currentFrame.value = frame
            if (previous != null && !acquiredFrames.containsKey(previous)) {
                skipped = previous
                skippedReleaseFence = takePendingReleaseFence(previous)
            }
        }
        skipped?.release(skippedReleaseFence)
    }

    internal fun attachConsumer(token: Any) {
        synchronized(lock) {
            check(!closed) { "TextureViewStreamController is closed" }
            check(consumerToken == null || consumerToken === token) {
                "TextureViewStreamController supports exactly one TextureView consumer"
            }
            consumerToken = token
        }
    }

    internal fun acquireFrame(
        token: Any,
        frame: TextureViewFrame,
    ): Boolean =
        synchronized(lock) {
            if (closed || consumerToken !== token || frame.isReleased) return@synchronized false
            acquiredFrames[frame] = (acquiredFrames[frame] ?: 0) + 1
            true
        }

    internal fun releaseFrame(
        frame: TextureViewFrame,
        releaseFenceFd: Int = NO_FENCE,
    ) {
        var release = false
        var fenceToRelease = NO_FENCE
        synchronized(lock) {
            val references = acquiredFrames[frame]
            if (references == null) {
                if (releaseFenceFd != NO_FENCE) closeAcquireFenceFd(releaseFenceFd)
                return
            }
            if (references > 1) {
                acquiredFrames[frame] = references - 1
                rememberReleaseFence(frame, releaseFenceFd)
            } else {
                acquiredFrames.remove(frame)
                release = closed || currentFrame.value !== frame
                if (release) {
                    fenceToRelease = newestReleaseFence(takePendingReleaseFence(frame), releaseFenceFd)
                } else {
                    rememberReleaseFence(frame, releaseFenceFd)
                }
            }
        }
        if (release) {
            frame.release(fenceToRelease)
        }
    }

    internal fun detachConsumer(token: Any) {
        val release = ArrayList<Pair<TextureViewFrame, Int>>()
        synchronized(lock) {
            if (consumerToken !== token) return
            consumerToken = null
            currentFrame.value?.takeUnless(acquiredFrames::containsKey)?.let { frame ->
                release += frame to takePendingReleaseFence(frame)
            }
            currentFrame.value = null
        }
        releaseIdentityDistinct(release)
    }

    /**
     * Drops the currently queued frame without closing the stream.
     *
     * A frame already acquired by the consumer stays alive until that consumer
     * releases it; an unacquired frame is released synchronously.
     */
    public fun clear() {
        var release: TextureViewFrame? = null
        var releaseFence = NO_FENCE
        synchronized(lock) {
            if (closed) return
            val current = currentFrame.value
            currentFrame.value = null
            if (current != null && !acquiredFrames.containsKey(current)) {
                release = current
                releaseFence = takePendingReleaseFence(current)
            }
        }
        release?.release(releaseFence)
    }

    /** Releases the queued frame and makes further submissions fail. */
    override fun close() {
        val release = ArrayList<Pair<TextureViewFrame, Int>>()
        synchronized(lock) {
            if (closed) return
            closed = true
            consumerToken = null
            currentFrame.value?.takeUnless(acquiredFrames::containsKey)?.let { frame ->
                release += frame to takePendingReleaseFence(frame)
            }
            currentFrame.value = null
        }
        releaseIdentityDistinct(release)
    }

    private fun releaseIdentityDistinct(frames: List<Pair<TextureViewFrame, Int>>) {
        val seen = IdentityHashMap<TextureViewFrame, Unit>(frames.size)
        frames.forEach { (frame, fenceFd) ->
            if (seen.put(frame, Unit) == null) {
                frame.release(fenceFd)
            } else if (fenceFd != NO_FENCE) {
                closeAcquireFenceFd(fenceFd)
            }
        }
    }

    /** Guarded by [lock]. A later fence on the same consumer queue subsumes the older one. */
    private fun rememberReleaseFence(
        frame: TextureViewFrame,
        fenceFd: Int,
    ) {
        if (fenceFd == NO_FENCE) return
        val previous = pendingReleaseFences.put(frame, fenceFd)
        if (previous != null && previous != NO_FENCE) closeAcquireFenceFd(previous)
    }

    /** Guarded by [lock]. */
    private fun takePendingReleaseFence(frame: TextureViewFrame): Int =
        pendingReleaseFences.remove(frame) ?: NO_FENCE

    private fun newestReleaseFence(
        previous: Int,
        newest: Int,
    ): Int {
        if (newest == NO_FENCE) return previous
        if (previous != NO_FENCE) closeAcquireFenceFd(previous)
        return newest
    }
}

/** Remembers and closes a stream controller with the current composition. */
@Composable
public fun rememberTextureViewStreamController(): TextureViewStreamController {
    val controller = remember { TextureViewStreamController() }
    DisposableEffect(controller) {
        onDispose(controller::close)
    }
    return controller
}

/** Composites the newest frame of a rotating-buffer [streamController]. */
@Composable
public fun TextureView(
    streamController: TextureViewStreamController,
    modifier: Modifier = Modifier,
    filterQuality: FilterQuality = FilterQuality.Low,
    contentScale: ContentScale = ContentScale.FillBounds,
    alignment: Alignment = Alignment.Center,
) {
    val consumer = remember(streamController) { TextureViewStreamConsumerLease(streamController) }
    val frame = streamController.currentFrame.value
    if (frame == null) {
        Box(modifier)
        return
    }
    val frameLease = remember(consumer, frame) { TextureViewStreamFrameLease(streamController, consumer.token, frame) }
    if (!frameLease.acquired) {
        Box(modifier)
        return
    }
    TextureView(
        source = frame.source,
        modifier = modifier,
        controller = frameLease.frameController,
        filterQuality = filterQuality,
        contentScale = contentScale,
        alignment = alignment,
    )
}

private class TextureViewStreamConsumerLease(
    private val streamController: TextureViewStreamController,
) : RememberObserver {
    val token: Any = Any()

    init {
        streamController.attachConsumer(token)
    }

    override fun onRemembered() = Unit

    override fun onForgotten() {
        streamController.detachConsumer(token)
    }

    override fun onAbandoned() {
        streamController.detachConsumer(token)
    }
}

private class TextureViewStreamFrameLease(
    private val streamController: TextureViewStreamController,
    token: Any,
    private val frame: TextureViewFrame,
) : RememberObserver {
    val acquired: Boolean = streamController.acquireFrame(token, frame)
    val frameController: TextureViewController? =
        if (acquired) {
            TextureViewController().also { it.markFrameAvailable(frame.takeAcquireFence()) }
        } else {
            null
        }

    override fun onRemembered() = Unit

    override fun onForgotten() = release()

    override fun onAbandoned() = release()

    private fun release() {
        if (!acquired) return
        val releaseFenceFd = frameController?.takeReleaseFence() ?: NO_FENCE
        frameController?.releaseAcquireFence()
        streamController.releaseFrame(frame, releaseFenceFd)
    }
}
