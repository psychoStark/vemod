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
            try {
                val currentProc = getProcessName()
                if (currentProc != packageName) {
                    android.webkit.WebView.setDataDirectorySuffix(currentProc.replace(":", "_"))
                }
                android.webkit.CookieManager.getInstance()
                Log.i("VemodApp", "Successfully primed CookieManager on main thread")
            } catch (e: Throwable) {
                Log.w("VemodApp", "Failed to initialize WebView/CookieManager", e)
            }
        }
        extractSysroot()
        val prefs = getSharedPreferences("vemod_settings", android.content.Context.MODE_PRIVATE)
        val logLevel = prefs.getInt("log_level", com.vemod.container.bridge.VemodBridge.LOG_LEVEL_BASIC)
        com.vemod.container.bridge.VemodBridge.nativeSetLogLevel(logLevel)

        com.vemod.container.proxy.VemodInstrumentation.install()
        com.vemod.container.proxy.PackageManagerHook.install(this)
        com.vemod.container.proxy.ActivityManagerHook.install(this)
        com.vemod.container.proxy.NotificationManagerHook.install(this)
        com.vemod.container.proxy.WindowSessionHook.install(packageName)

        try {
            applicationInfo.targetSdkVersion = 23
            val am = getSystemService(android.content.Context.ALARM_SERVICE) as? android.app.AlarmManager
            if (am != null) {
                val field = android.app.AlarmManager::class.java.getDeclaredField("mTargetSdkVersion")
                field.isAccessible = true
                field.setInt(am, 23)
            }
        } catch (e: Throwable) {
            Log.w("VemodApp", "Failed to configure targetSdkVersion / AlarmManager", e)
        }

        val defaultHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            var t: Throwable? = throwable
            var shouldSwallow = false
            while (t != null) {
                val msg = t.message ?: ""
                val isAdOrGms = t.stackTrace.any { 
                    it.className.contains("basegameutils") || 
                    it.className.contains("google.android.gms") ||
                    it.className.contains("chartboost") ||
                    it.className.contains("unity3d.ads") ||
                    it.className.contains("applovin") ||
                    it.className.contains("mopub") ||
                    it.className.contains("vungle") ||
                    it.className.contains("adcolony") ||
                    it.className.contains("ironsource") ||
                    it.className.contains("facebook.ads") ||
                    it.className.contains("chromium") ||
                    it.className.contains("NotificationManager")
                }
                if (msg.contains("Service not registered") ||
                    msg.contains("Unknown calling package name") ||
                    msg.contains("Can't create handler inside thread") ||
                    msg.contains("not owned by uid") ||
                    msg.contains("org.apache.http") ||
                    isAdOrGms) {
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
               pkg.contains("google") ||
               action.contains("licensing", ignoreCase = true) ||
               pkg.contains("licensing", ignoreCase = true) ||
               pkg.contains("com.android.vending")
    }

    override fun bindService(service: android.content.Intent, conn: android.content.ServiceConnection, flags: Int): Boolean {
        val isGmsTarget = com.vemod.container.proxy.MicroGManager.isGmsTarget(service)
        val isBillingOrLicensing = service.action?.contains("billing", ignoreCase = true) == true ||
                                   service.action?.contains("licensing", ignoreCase = true) == true

        if (isBillingOrLicensing) {
            if (com.vemod.container.proxy.MicroGManager.isMicroGInstalled(this)) {
                val rewritten = com.vemod.container.proxy.MicroGManager.rewriteGmsIntent(service, this)
                val canResolve = packageManager.resolveService(rewritten, 0) != null
                if (canResolve) {
                    return try {
                        super.bindService(rewritten, conn, flags)
                    } catch (e: Throwable) {
                        Log.w("VemodApp", "Delegated service bind to microG billing failed, falling back to mock: ${e.message}")
                        com.vemod.container.proxy.MockBillingService.mockBind(service, conn)
                    }
                }
            }
            return com.vemod.container.proxy.MockBillingService.mockBind(service, conn)
        }

        if (isGmsTarget) {
            if (com.vemod.container.proxy.MicroGManager.isMicroGInstalled(this)) {
                val rewritten = com.vemod.container.proxy.MicroGManager.rewriteGmsIntent(service, this)
                Log.i("VemodApp", "Delegating application bindService directly to host microG: $rewritten")
                return try {
                    super.bindService(rewritten, conn, flags)
                } catch (e: Throwable) {
                    Log.w("VemodApp", "Delegated application bindService to microG failed: ${e.message}")
                    false
                }
            } else {
                return com.vemod.container.proxy.MockBillingService.mockBind(service, conn)
            }
        }
        if (isGoogleService(service)) {
            Log.w("VemodApp", "Preventing Application.bindService to Google service: $service")
            return false
        }
        return super.bindService(service, conn, flags)
    }

    override fun startService(service: android.content.Intent): android.content.ComponentName? {
        val isGmsTarget = com.vemod.container.proxy.MicroGManager.isGmsTarget(service)
        if (isGmsTarget) {
            if (com.vemod.container.proxy.MicroGManager.isMicroGInstalled(this)) {
                val rewritten = com.vemod.container.proxy.MicroGManager.rewriteGmsIntent(service, this)
                return try {
                    super.startService(rewritten)
                } catch (e: Throwable) {
                    Log.w("VemodApp", "Delegated startService to microG failed: ${e.message}")
                    null
                }
            }
            return null
        }
        if (isGoogleService(service)) {
            Log.w("VemodApp", "Preventing Application.startService to Google service: $service")
            return null
        }
        return super.startService(service)
    }

    private fun extractSysroot() {
        try {
            com.vemod.container.container.SysrootManager.ensureSysroot(this)
        } catch (e: Throwable) {
            Log.e("VemodApp", "Failed to initialize sysroot", e)
        }
    }
}
