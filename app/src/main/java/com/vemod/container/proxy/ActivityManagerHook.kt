package com.vemod.container.proxy

import android.app.PendingIntent
import android.content.Context
import android.util.Log
import java.lang.reflect.InvocationHandler
import java.lang.reflect.Method
import java.lang.reflect.Proxy

class ActivityManagerHook(
    private val real: Any,
    private val hostPackage: String
) : InvocationHandler {

    companion object {
        private const val TAG = "ActivityManagerHook"

        fun install(context: Context) {
            try {
                val amClass = Class.forName("android.app.ActivityManager")
                val singletonField = try {
                    amClass.getDeclaredField("IActivityManagerSingleton")
                } catch (_: NoSuchFieldException) {
                    amClass.getDeclaredField("gDefault")
                }
                singletonField.isAccessible = true
                val singleton = singletonField.get(null) ?: return

                val singletonClass = Class.forName("android.util.Singleton")
                val getMethod = singletonClass.getDeclaredMethod("get")
                getMethod.isAccessible = true
                val realIam = getMethod.invoke(singleton) ?: return

                if (Proxy.isProxyClass(realIam.javaClass)) {
                    Log.i(TAG, "ActivityManager already hooked")
                    return
                }

                val iamClass = Class.forName("android.app.IActivityManager")
                val proxy = Proxy.newProxyInstance(
                    iamClass.classLoader,
                    arrayOf(iamClass),
                    ActivityManagerHook(realIam, context.packageName)
                )

                val mInstanceField = singletonClass.getDeclaredField("mInstance")
                mInstanceField.isAccessible = true
                mInstanceField.set(singleton, proxy)

                Log.i(TAG, "ActivityManagerHook successfully installed")
            } catch (e: Throwable) {
                Log.e(TAG, "Failed to install ActivityManagerHook", e)
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

        if (args != null && method.name.contains("IntentSender", ignoreCase = true)) {
            // Fix packageName argument so AMS doesn't throw Permission Denial
            for (i in args.indices) {
                val arg = args[i]
                if (arg is String && arg != hostPackage && arg.isNotEmpty() && !arg.startsWith("android.")) {
                    args[i] = hostPackage
                    Log.i(TAG, "${method.name}: patched packageName '$arg' -> '$hostPackage'")
                    break
                }
            }
            // Ensure PendingIntent mutability flag is present on Android 12+ (SDK 31+).
            // In getIntentSenderWithFeature(..., int flags, Bundle options, int userId):
            // Parameter index of 'flags' is args.size - 3 (or the Int before options and userId).
            // Do NOT mutate 'userId' (the last parameter) or 'type' / 'requestCode'!
            val flagsIndex = args.indexOfLast { it is Int && args.indexOf(it) != args.size - 1 }
            if (flagsIndex in 0 until args.size - 1) {
                val arg = args[flagsIndex] as Int
                val immutable = PendingIntent.FLAG_IMMUTABLE
                val mutable = PendingIntent.FLAG_MUTABLE
                if ((arg and (immutable or mutable)) == 0) {
                    args[flagsIndex] = arg or immutable
                    Log.i(TAG, "${method.name}: patched flags at index $flagsIndex: $arg -> ${args[flagsIndex]}")
                }
            }
        }

        if (args != null && method.name.startsWith("registerReceiver")) {
            // Patch callerPackage to hostPackage so AMS does not reject caller
            for (i in args.indices) {
                val arg = args[i]
                if (arg is String && arg != hostPackage && arg.isNotEmpty() && !arg.startsWith("android.")) {
                    args[i] = hostPackage
                    Log.i(TAG, "${method.name}: patched callerPackage '$arg' -> '$hostPackage'")
                    break
                }
            }
            // Patch receiver flags on Android 14+ (SDK 34+) to include RECEIVER_EXPORTED (0x2)
            if (android.os.Build.VERSION.SDK_INT >= 33) {
                for (i in args.indices.reversed()) {
                    val arg = args[i]
                    if (arg is Int) {
                        val receiverExported = 0x2
                        val receiverNotExported = 0x4
                        if ((arg and (receiverExported or receiverNotExported)) == 0) {
                            args[i] = arg or receiverExported
                            Log.i(TAG, "${method.name}: patched receiver flags $arg -> ${args[i]}")
                        }
                        break
                    }
                }
            }
        }

        return try {
            if (args != null) method.invoke(real, *args) else method.invoke(real)
        } catch (e: Throwable) {
            val cause = e.cause ?: e
            if (cause is SecurityException || cause is IllegalArgumentException || cause is android.os.RemoteException) {
                if (method.name.startsWith("registerReceiver") || method.name.contains("IntentSender", ignoreCase = true)) {
                    Log.w(TAG, "Suppressed ${cause.javaClass.simpleName} in ${method.name}: ${cause.message}")
                    return when (method.returnType) {
                        Boolean::class.javaPrimitiveType -> false
                        Int::class.javaPrimitiveType -> 0
                        Long::class.javaPrimitiveType -> 0L
                        else -> null
                    }
                }
            }
            throw cause
        }
    }
}
