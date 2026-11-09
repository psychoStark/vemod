package com.vemod.container

import android.app.Application
import android.os.Build
import android.util.Log
import org.lsposed.hiddenapibypass.HiddenApiBypass

class VemodApp : Application() {

    companion object {
        @Volatile
        var instance: VemodApp? = null
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            try {
                HiddenApiBypass.addHiddenApiExemptions("")
                Log.i("VemodApp", "HiddenApi exemptions applied")
            } catch (e: Throwable) {
                Log.w("VemodApp", "Failed to apply hidden API exemptions", e)
            }
        }
        com.vemod.container.proxy.VemodInstrumentation.install()
        com.vemod.container.proxy.PackageManagerHook.install(this)

        val defaultHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            var t: Throwable? = throwable
            var shouldSwallow = false
            while (t != null) {
                val msg = t.message ?: ""
                val isGms = t.stackTrace.any { 
                    it.className.contains("basegameutils") || 
                    it.className.contains("google.android.gms") ||
                    it.className.contains("chromium")
                }
                if (msg.contains("Service not registered") ||
                    msg.contains("Unknown calling package name") ||
                    msg.contains("Can't create handler inside thread") ||
                    isGms) {
                    shouldSwallow = true
                    break
                }
                t = t.cause
            }
            if (shouldSwallow) {
                Log.w("VemodApp", "Swallowed background exception on thread ${thread.name}: ${throwable.message}")
                if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) {
                    while (true) {
                        try {
                            android.os.Looper.loop()
                            break
                        } catch (le: Throwable) {
                            Log.w("VemodApp", "Recovered main looper crash: ${le.message}")
                        }
                    }
                }
                return@setDefaultUncaughtExceptionHandler
            }
            defaultHandler?.uncaughtException(thread, throwable)
        }
    }

    override fun getResources(): android.content.res.Resources {
        com.vemod.container.proxy.VemodInstrumentation.guestResources?.let { return it }
        return super.getResources()
    }

    override fun unbindService(conn: android.content.ServiceConnection) {
        try {
            super.unbindService(conn)
        } catch (e: IllegalArgumentException) {
            Log.w("VemodApp", "Swallowed unbindService exception: ${e.message}")
        }
    }

    private fun isGoogleService(service: android.content.Intent?): Boolean {
        if (service == null) return false
        val pkg = service.component?.packageName ?: service.`package` ?: ""
        val action = service.action ?: ""
        return pkg.contains("com.google.android.gms") ||
               action.contains("com.google.android.gms") ||
               pkg.contains("google")
    }

    override fun bindService(service: android.content.Intent, conn: android.content.ServiceConnection, flags: Int): Boolean {
        if (isGoogleService(service)) {
            Log.w("VemodApp", "Preventing Application.bindService to Google service: $service")
            return false
        }
        return super.bindService(service, conn, flags)
    }

    override fun startService(service: android.content.Intent): android.content.ComponentName? {
        if (isGoogleService(service)) {
            Log.w("VemodApp", "Preventing Application.startService to Google service: $service")
            return null
        }
        return super.startService(service)
    }
}
