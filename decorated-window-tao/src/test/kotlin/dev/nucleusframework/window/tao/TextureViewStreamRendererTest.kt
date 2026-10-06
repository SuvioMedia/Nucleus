package dev.nucleusframework.window.tao

import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.drawscope.DrawScope
import dev.nucleusframework.window.tao.scene.runTaoSceneTest
import org.jetbrains.skia.ColorFilter
import org.jetbrains.skia.Image
import kotlin.test.Test
import kotlin.test.assertEquals

class TextureViewStreamRendererTest {
    @Test
    fun switchingCustomRendererKeepsTheSameStreamConsumer() {
        TextureViewStreamController().use { controller ->
            runTaoSceneTest(width = 32, height = 32) {
                var custom by mutableStateOf(false)
                var applied = 0
                val renderer =
                    object : TextureViewImageRenderer {
                        override fun DrawScope.drawFrame(
                            image: Image,
                            colorFilter: ColorFilter?,
                        ) = Unit
                    }
                setContent {
                    TextureView(controller, imageRenderer = if (custom) renderer else null)
                    SideEffect { applied++ }
                }
                custom = true
                frameUntilIdle()
                custom = false
                frameUntilIdle()
                assertEquals(3, applied)
            }
        }
    }
}
