package com.vemod.container.proxy

import android.app.NotificationManager
import android.content.Context
import android.util.Log
import java.lang.reflect.InvocationHandler
import java.lang.reflect.Method
import java.lang.reflect.Proxy

class NotificationManagerHook(
    private val real: Any,
    private val hostPackage: String
) : InvocationHandler {

    companion object {
        private const val TAG = "NotificationManagerHook"

        fun install(context: Context) {
            try {
                val nmClass = Class.forName("android.app.NotificationManager")
                val getServiceMethod = try {
                    nmClass.getDeclaredMethod("getService")
                } catch (_: NoSuchMethodException) {
                    null
                }
                val realService = if (getServiceMethod != null) {
                    getServiceMethod.isAccessible = true
                    getServiceMethod.invoke(null)
                } else {
                    val sServiceField = nmClass.getDeclaredField("sService")
                    sServiceField.isAccessible = true
                    sServiceField.get(null)
                } ?: return

                if (Proxy.isProxyClass(realService.javaClass)) {
                    Log.i(TAG, "NotificationManager already hooked")
                    return
                }

                val inmClass = Class.forName("android.app.INotificationManager")
                val proxy = Proxy.newProxyInstance(
                    inmClass.classLoader,
                    arrayOf(inmClass),
                    NotificationManagerHook(realService, context.packageName)
                )

                val sServiceField = nmClass.getDeclaredField("sService")
                sServiceField.isAccessible = true
                sServiceField.set(null, proxy)

                Log.i(TAG, "NotificationManagerHook successfully installed")
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to install NotificationManagerHook", e)
            }
        }
    }

    override fun invoke(proxy: Any, method: Method, args: Array<Any?>?): Any? {
        if (method.name == "equals") {
            val other = args?.getOrNull(0)
            return proxy === other || real == other
        }
        if (method.name == "hashCode") {
            return real.hashCode()
        }
        if (method.name == "toString") {
            return real.toString()
        }

        if (args != null) {
            for (i in args.indices) {
                val arg = args[i]
                if (arg is String && (arg.contains(".") && !arg.startsWith(hostPackage))) {
                    // Rewrite guest package name to host package name to prevent UID ownership checks failing
                    args[i] = hostPackage
                }
            }
        }

        return try {
            method.invoke(real, *(args ?: emptyArray()))
        } catch (e: Throwable) {
            val cause = e.cause ?: e
            if (cause is SecurityException) {
                Log.w(TAG, "Suppressed SecurityException in INotificationManager.${method.name}: ${cause.message}")
                when (method.returnType) {
                    Boolean::class.javaPrimitiveType -> false
                    Int::class.javaPrimitiveType -> 0
                    Long::class.javaPrimitiveType -> 0L
                    List::class.java -> emptyList<Any>()
                    else -> null
                }
            } else {
                throw cause
            }
        }
    }
}
