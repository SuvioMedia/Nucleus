package dev.nucleusframework.window.tao

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

class TextureViewStreamControllerTest {
    @Test
    fun submittingNewFrameReleasesSkippedFrame() {
        val releases = mutableListOf<String>()
        val stream = TextureViewStreamController()
        val first = frame(1) { releases += "first" }
        val second = frame(2) { releases += "second" }

        stream.submitFrame(first)
        stream.submitFrame(second)

        assertEquals(listOf("first"), releases)
        stream.close()
        assertEquals(listOf("first", "second"), releases)
    }

    @Test
    fun acquiredFrameLivesUntilConsumerReleasesIt() {
        var firstReleases = 0
        var secondReleases = 0
        val stream = TextureViewStreamController()
        val token = Any()
        val first = frame(1) { firstReleases++ }
        val second = frame(2) { secondReleases++ }

        stream.attachConsumer(token)
        stream.submitFrame(first)
        assertTrue(stream.acquireFrame(token, first))
        stream.submitFrame(second)

        assertEquals(0, firstReleases)
        stream.releaseFrame(first)
        assertEquals(1, firstReleases)

        stream.detachConsumer(token)
        assertEquals(1, secondReleases)
        stream.close()
        assertEquals(1, secondReleases)
    }

    @Test
    fun closingWithCurrentAcquiredFrameReleasesItExactlyOnce() {
        var releases = 0
        val stream = TextureViewStreamController()
        val token = Any()
        val frame = frame(1) { releases++ }

        stream.attachConsumer(token)
        stream.submitFrame(frame)
        assertTrue(stream.acquireFrame(token, frame))

        stream.close()
        stream.releaseFrame(frame)

        assertEquals(1, releases)
    }

    @Test
    fun streamRejectsSecondConsumerAndFrameResubmission() {
        val stream = TextureViewStreamController()
        val firstToken = Any()
        stream.attachConsumer(firstToken)

        assertFailsWith<IllegalStateException> { stream.attachConsumer(Any()) }

        val frame = frame(1) {}
        stream.submitFrame(frame)
        assertFailsWith<IllegalStateException> { stream.submitFrame(frame) }
        stream.close()
    }

    @Test
    fun extendedLinearMetadataKeepsReferenceWhiteSemantic() {
        val info =
            TextureColorInfo(
                encoding = TextureColorEncoding.EXTENDED_LINEAR_SRGB,
                premultipliedAlpha = true,
                sdrWhiteLevelNits = 203f,
            )

        assertEquals(TextureColorEncoding.EXTENDED_LINEAR_SRGB, info.encoding)
        assertEquals(203f, info.sdrWhiteLevelNits)
    }

    @Test
    fun referenceWhiteScalePreservesExtendedLinearSceneUnits() {
        val info =
            TextureColorInfo(
                encoding = TextureColorEncoding.EXTENDED_LINEAR_SRGB,
                sdrWhiteLevelNits = 203f,
            )

        val scale = textureReferenceWhiteScale(info, hostSdrWhiteLevelNits = 80f)
        val sceneValues = listOf(-1f, 1f, 4f, 12.5f).map { component -> component * scale }

        assertEquals(203f / 80f, scale)
        listOf(-2.5375f, 2.5375f, 10.15f, 31.71875f)
            .zip(sceneValues)
            .forEach { (expected, actual) -> assertEquals(expected, actual, absoluteTolerance = 0.00001f) }
        assertTrue(sceneValues.first() < 0f)
        assertTrue(sceneValues.last() > 1f)
    }

    @Test
    fun referenceWhiteScaleIsIdentityForSrgbOrHostRelativeProducer() {
        assertEquals(1f, textureReferenceWhiteScale(TextureColorInfo.SRGB_PREMULTIPLIED, 80f))
        assertEquals(
            1f,
            textureReferenceWhiteScale(TextureColorInfo.EXTENDED_LINEAR_SRGB_PREMULTIPLIED, 80f),
        )
    }

    private fun frame(
        handle: Long,
        released: () -> Unit,
    ): TextureViewFrame =
        TextureViewFrame(
            source =
                nucleusD3D11SharedTextureSource(
                    sharedHandle = handle,
                    widthPx = 1,
                    heightPx = 1,
                    colorInfo = TextureColorInfo.EXTENDED_LINEAR_SRGB_PREMULTIPLIED,
                ),
            onReleased = { released() },
        )
}
