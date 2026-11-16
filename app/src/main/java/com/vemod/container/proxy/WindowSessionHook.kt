package com.vemod.container.proxy

import android.util.Log
import android.view.WindowManager
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Proxy

object WindowSessionHook {
    private const val TAG = "WindowSessionHook"
    private var installed = false

    fun install(hostPackageName: String) {
        if (installed) return
        try {
            val wmgClass = Class.forName("android.view.WindowManagerGlobal")
            val getSessionMethod = wmgClass.getDeclaredMethod("getWindowSession")
            getSessionMethod.isAccessible = true
            val realSession = getSessionMethod.invoke(null) ?: return

            if (Proxy.isProxyClass(realSession.javaClass)) {
                installed = true
                return
            }

            val sessionClass = Class.forName("android.view.IWindowSession")
            val proxy = Proxy.newProxyInstance(
                sessionClass.classLoader,
                arrayOf(sessionClass)
            ) { _, method, args ->
                if (args != null) {
                    for (i in args.indices) {
                        val arg = args[i]
                        if (arg is WindowManager.LayoutParams) {
                            if (arg.packageName != null && arg.packageName != hostPackageName) {
                                Log.i(TAG, "WindowSessionHook: sanitized LayoutParams.packageName '${arg.packageName}' -> '$hostPackageName' in ${method.name}")
                                arg.packageName = hostPackageName
                            }
                        }
                    }
                }
                try {
                    method.invoke(realSession, *(args ?: emptyArray()))
                } catch (e: InvocationTargetException) {
                    throw e.targetException
                }
            }

            val sSessionField = wmgClass.getDeclaredField("sWindowSession")
            sSessionField.isAccessible = true
            sSessionField.set(null, proxy)
            installed = true
            Log.i(TAG, "WindowSessionHook installed successfully")
        } catch (e: Throwable) {
            Log.w(TAG, "Failed to install WindowSessionHook", e)
        }
    }
}
