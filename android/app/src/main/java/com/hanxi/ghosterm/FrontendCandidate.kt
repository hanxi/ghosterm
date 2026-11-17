package com.hanxi.ghosterm

import android.os.SystemClock
import com.hanxi.ghosterm.bridge.NativeTerminalSession
import java.util.UUID

/** Output source is known; execution target is unverified and never inferred. */
data class FrontendCandidate(val id: UUID, val sourceID: UUID, val sourceDescription: String,
    val offer: NativeTerminalSession.DiscoveryOffer, val receivedAt: Long) {
    val expired get() = SystemClock.elapsedRealtime() - receivedAt >= 30000
}
