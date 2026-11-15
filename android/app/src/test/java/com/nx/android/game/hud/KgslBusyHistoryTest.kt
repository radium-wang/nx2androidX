package com.nx.android.game.hud

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class KgslBusyHistoryTest {
    @Test fun repeatedWindowExpiresAndFreshWindowRecovers() {
        val samples = KgslBusyHistory(5000)
        assertEquals(6f, samples.update(60, 1000, 0))
        assertEquals(6f, samples.update(60, 1000, 4999))
        assertNull(samples.update(60, 1000, 5000))
        assertEquals(45f, samples.update(450, 1000, 6000))
    }

    @Test fun equalPercentWithNewCountersRemainsFresh() {
        val samples = KgslBusyHistory(5000)
        assertEquals(6f, samples.update(60, 1000, 0))
        assertEquals(6f, samples.update(120, 2000, 4000))
        assertEquals(6f, samples.update(0, 0, 8000))
        samples.clear()
        assertNull(samples.update(0, 0, 8001))
    }
}
