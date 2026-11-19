package com.vemod.container.proxy

import android.app.Activity
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.MotionEvent
import android.view.ViewConfiguration
import android.view.Window
import java.lang.reflect.InvocationHandler
import java.lang.reflect.Method
import java.lang.reflect.Proxy

/**
 * Paces touch events for guest games running on high-refresh-rate displays.
 *
 * On modern 120Hz/240Hz screens, quick taps often deliver ACTION_DOWN and ACTION_UP
 * within a few milliseconds, causing both events to be queued and dispatched to the
 * game engine in the exact same render frame. In engines like Rovio Fusion (Angry Birds),
 * if TouchBegan and TouchEnded arrive in the same frame without an engine update tick,
 * the button transitions to pressed/held state without firing its click callback.
 *
 * TouchPacer ensures that quick taps maintain a minimum duration of at least 60ms
 * between ACTION_DOWN and ACTION_UP by delaying ACTION_UP to the next frame when necessary,
 * while preserving immediate dispatch for drags, flings, and long presses.
 */
class TouchPacer private constructor(
    private val activity: Activity,
    private val delegate: Window.Callback
) : InvocationHandler {

    companion object {
        private const val TAG = "TouchPacer"
        private const val MIN_TAP_DURATION_MS = 60L

        fun install(activity: Activity) {
            try {
                val window = activity.window ?: return
                val currentCallback = window.callback ?: activity
                val pacer = TouchPacer(activity, currentCallback)
                val proxy = Proxy.newProxyInstance(
                    Window.Callback::class.java.classLoader,
                    arrayOf(Window.Callback::class.java),
                    pacer
                ) as Window.Callback
                window.callback = proxy
                Log.i(TAG, "TouchPacer installed for ${activity.javaClass.name}")
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to install TouchPacer for ${activity.javaClass.name}", e)
            }
        }
    }

    private val handler = Handler(Looper.getMainLooper())
    private val touchSlop = ViewConfiguration.get(activity).scaledTouchSlop.toFloat()

    private var lastDownTime = 0L
    private var downX = 0f
    private var downY = 0f
    private var isDragging = false

    private var pendingUp: MotionEvent? = null
    private var pendingRunnable: Runnable? = null

    private fun flushPendingUp() {
        pendingRunnable?.let { handler.removeCallbacks(it) }
        pendingRunnable = null
        val up = pendingUp
        pendingUp = null
        if (up != null) {
            try {
                delegate.dispatchTouchEvent(up)
            } catch (e: Throwable) {
                Log.w(TAG, "Error flushing pending up event", e)
            } finally {
                up.recycle()
            }
        }
    }

    override fun invoke(proxy: Any?, method: Method, args: Array<out Any?>?): Any? {
        if (method.name == "equals") {
            val other = args?.getOrNull(0)
            return proxy === other || delegate == other
        }
        if (method.name == "hashCode") {
            return delegate.hashCode()
        }
        if (method.name == "toString") {
            return delegate.toString()
        }

        if (method.name == "dispatchTouchEvent" && args != null && args.isNotEmpty()) {
            val event = args[0] as? MotionEvent
            if (event != null) {
                return handleDispatchTouchEvent(event)
            }
        }
        return try {
            if (args == null || args.isEmpty()) {
                method.invoke(delegate)
            } else {
                method.invoke(delegate, *args)
            }
        } catch (e: java.lang.reflect.InvocationTargetException) {
            throw e.targetException ?: e
        }
    }

    private fun handleDispatchTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                flushPendingUp()
                lastDownTime = SystemClock.uptimeMillis()
                downX = event.x
                downY = event.y
                isDragging = false
                return delegate.dispatchTouchEvent(event)
            }

            MotionEvent.ACTION_MOVE -> {
                val dx = Math.abs(event.x - downX)
                val dy = Math.abs(event.y - downY)
                if (dx > touchSlop || dy > touchSlop) {
                    isDragging = true
                    flushPendingUp()
                }
                return delegate.dispatchTouchEvent(event)
            }

            MotionEvent.ACTION_UP -> {
                val now = SystemClock.uptimeMillis()
                val elapsed = now - lastDownTime

                // If this was a quick tap (not dragging, and released within MIN_TAP_DURATION_MS)
                if (!isDragging && elapsed < MIN_TAP_DURATION_MS) {
                    flushPendingUp()
                    val copy = MotionEvent.obtain(event)
                    pendingUp = copy
                    val delay = MIN_TAP_DURATION_MS - elapsed

                    val r = Runnable {
                        if (pendingUp === copy) {
                            pendingUp = null
                            pendingRunnable = null
                            try {
                                delegate.dispatchTouchEvent(copy)
                            } catch (e: Throwable) {
                                Log.w(TAG, "Error dispatching paced up event", e)
                            } finally {
                                copy.recycle()
                            }
                        }
                    }
                    pendingRunnable = r
                    handler.postDelayed(r, delay)
                    return true
                } else {
                    flushPendingUp()
                    return delegate.dispatchTouchEvent(event)
                }
            }

            MotionEvent.ACTION_CANCEL,
            MotionEvent.ACTION_POINTER_DOWN -> {
                flushPendingUp()
                return delegate.dispatchTouchEvent(event)
            }

            else -> {
                return delegate.dispatchTouchEvent(event)
            }
        }
    }
}
