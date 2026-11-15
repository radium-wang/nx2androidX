package com.nx.android.game.hud

/** The sysfs node can return the same completed window repeatedly. */
internal class KgslBusyHistory(private val holdMs: Long) {
    private data class Window(val busy: Long, val total: Long)
    private var window: Window? = null
    private var changedAt = 0L

    fun update(busy: Long, total: Long, now: Long): Float? {
        if (total > 0L && busy in 0L..total) {
            val current = Window(busy, total)
            if (current != window) {
                window = current
                changedAt = now
            }
        }
        return window?.takeIf { now - changedAt < holdMs }?.let { it.busy * 100f / it.total }
    }

    fun clear() {
        window = null
    }
}
