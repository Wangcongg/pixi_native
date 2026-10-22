package org.pixinative.runtime

/** Controls native frame admission only. JS, transitions and timers keep their real clocks. */
internal class NativeFramePacer(initialReason: String? = null, private val adaptive: Boolean = true,
                                overrideFps: Int? = null) {
    var targetFps: Int = overrideFps ?: if (initialReason != null) 30 else 0
        private set
    var reason: String = if (overrideFps != null) "debug-override" else initialReason ?: "display-vsync"
        private set
    private var nextFrameNanos: Long? = null
    private var warmupUntilNanos: Long? = null
    private var lastSampleNanos: Long? = null
    private var windowStartNanos: Long? = null
    private var frames = 0
    private var expensiveFrames = 0
    private var slowWindows = 0

    init { require(targetFps in setOf(0, 30, 60)) }

    companion object {
        private const val SECOND = 1_000_000_000L
        private const val VSYNC_TOLERANCE = 1_000_000L

        fun deviceReason(manufacturer: String, model: String, lowRam: Boolean): String? = when {
            // This exact Fire HD 10 was profiled at 20–40 FPS in the native Pixi machines.
            manufacturer.equals("Amazon", ignoreCase = true) && model.equals("KFTUWI", ignoreCase = true) ->
                "profiled-fire-hd10"
            lowRam -> "android-low-ram"
            else -> null
        }
    }

    fun resetForSurface() {
        nextFrameNanos = null
        warmupUntilNanos = null
        lastSampleNanos = null
        resetEvidence()
    }

    fun shouldRender(frameTimeNanos: Long): Boolean {
        if (targetFps == 0) return true
        val interval = SECOND / targetFps
        val deadline = nextFrameNanos
        if (deadline == null || frameTimeNanos - deadline > SECOND) {
            nextFrameNanos = frameTimeNanos + interval
            return true
        }
        if (frameTimeNanos + VSYNC_TOLERANCE < deadline) return false
        // Keep an absolute cadence. Counting callbacks would turn an already missed VSync
        // into a second skipped frame (30 -> 15 FPS); overdue slots are never replayed.
        nextFrameNanos = deadline + ((frameTimeNanos + VSYNC_TOLERANCE - deadline) / interval + 1) * interval
        return true
    }

    /** Returns true once when sustained, expensive rendering selects the 30 FPS tier. */
    fun onFrameCompleted(frameTimeNanos: Long, drawNanos: Long, presented: Boolean): Boolean {
        if (!adaptive || targetFps != 0) return false
        if (warmupUntilNanos == null) warmupUntilNanos = frameTimeNanos + 8 * SECOND
        val previous = lastSampleNanos
        lastSampleNanos = frameTimeNanos
        if (!presented || drawNanos >= 100_000_000L ||
            (previous != null && frameTimeNanos - previous > 250_000_000L)) {
            // Static screens, Unity suspension, app backgrounding and isolated load/GC stalls
            // are not evidence of a slow device. Require fresh continuous rendering afterwards.
            resetEvidence()
            warmupUntilNanos = maxOf(warmupUntilNanos!!, frameTimeNanos + 2 * SECOND)
            return false
        }
        if (frameTimeNanos < warmupUntilNanos!!) return false
        if (windowStartNanos == null) windowStartNanos = frameTimeNanos
        frames++
        if (drawNanos >= 20_000_000L) expensiveFrames++
        val elapsed = frameTimeNanos - windowStartNanos!!
        if (elapsed < 4 * SECOND) return false
        val fps = (frames - 1) * SECOND.toDouble() / elapsed
        val slow = frames >= 20 && fps < 45.0 && expensiveFrames * 2 >= frames
        slowWindows = if (slow) slowWindows + 1 else 0
        windowStartNanos = null
        frames = 0
        expensiveFrames = 0
        if (slowWindows < 3) return false
        targetFps = 30
        reason = "sustained-render-cost"
        nextFrameNanos = frameTimeNanos + SECOND / targetFps
        // Keep the tier for this Activity/session to avoid oscillating after load decreases.
        return true
    }

    private fun resetEvidence() {
        windowStartNanos = null
        frames = 0
        expensiveFrames = 0
        slowWindows = 0
    }
}
