package dev.nucleusframework.window.tao.scene

import kotlin.test.Test
import kotlin.test.assertEquals

class TaoInteropTransactionTest {
    @Test
    fun preparesComposeStateBeforeNativePresentationActions() {
        val events = mutableListOf<String>()
        val transaction = MutableTaoInteropTransaction(isInteropActive = true)
        transaction.addPreparation { events += "prepare-overlay-viewport" }
        transaction.add { events += "resize-native-view" }

        transaction.prepare()
        events += "record-compose-frame"
        transaction.performTransaction()

        assertEquals(
            listOf(
                "prepare-overlay-viewport",
                "record-compose-frame",
                "resize-native-view",
            ),
            events,
        )
    }
}
