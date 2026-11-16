package com.vemod.container.virtual

import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ApplicationInfo
import android.content.res.AssetManager
import android.content.res.Resources
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.util.Log
import com.vemod.container.proxy.VemodInstrumentation
import java.io.File

class GuestContext(
    base: Context,
    private val guestPackageName: String,
    private val containerDir: File,
    private val baseApk: File,
    private val guestLoader: ClassLoader,
    private val preloadedAssets: AssetManager? = null,
    private val preloadedResources: Resources? = null,
    private val isDataExposed: Boolean = false,
    private val exposedDir: File? = null
) : ContextWrapper(base) {

    private val guestDataDir: File = if (isDataExposed && exposedDir != null) {
        File(exposedDir, "data").apply { mkdirs() }
    } else {
        File(containerDir, "data").apply { mkdirs() }
    }

    private val guestFilesDir: File = File(guestDataDir, "files").apply { mkdirs() }
    private val guestCacheDir: File = File(guestDataDir, "cache").apply { mkdirs() }

    private val guestObbDir: File = if (isDataExposed && exposedDir != null) {
        File(exposedDir, "obb").apply { mkdirs() }
    } else {
        File(containerDir, "external/obb").apply { mkdirs() }
    }

    private val guestResourcesPair: Pair<AssetManager, Resources> by lazy {
        if (preloadedAssets != null && preloadedResources != null) {
            Pair(preloadedAssets, preloadedResources)
        } else {
            VemodInstrumentation.createGuestResources(base, baseApk)
        }
    }

    private val guestAssetManager: AssetManager
        get() = guestResourcesPair.first

    private val guestResources: Resources
        get() = guestResourcesPair.second

    private val guestAppInfo: ApplicationInfo by lazy {
        val parsedTargetSdk = try {
            base.packageManager.getPackageArchiveInfo(baseApk.absolutePath, 0)?.applicationInfo?.targetSdkVersion ?: 28
        } catch (_: Throwable) { 28 }

        ApplicationInfo(base.applicationInfo).apply {
            packageName = guestPackageName
            sourceDir = baseApk.absolutePath
            publicSourceDir = baseApk.absolutePath
            dataDir = guestDataDir.absolutePath
            nativeLibraryDir = File(containerDir, "proxy").absolutePath
            targetSdkVersion = if (parsedTargetSdk > 0) parsedTargetSdk else 28
            icon = 0
        }
    }

    var guestApplication: android.app.Application? = null

    override fun getPackageName(): String {
        val stack = Thread.currentThread().stackTrace
        for (i in 2 until minOf(stack.size, 10)) {
            val cls = stack[i].className
            if (cls.startsWith("android.media.") ||
                cls.startsWith("android.app.AppOpsManager") ||
                cls.startsWith("android.view.") ||
                cls.startsWith("android.os.Vibrator") ||
                cls.startsWith("android.os.SystemVibrator")) {
                return baseContext.packageName
            }
        }
        return guestPackageName
    }
    override fun getOpPackageName(): String = baseContext.opPackageName
    override fun getAttributionSource(): android.content.AttributionSource {
        return if (android.os.Build.VERSION.SDK_INT >= 31) {
            baseContext.attributionSource
        } else {
            super.getAttributionSource()
        }
    }
    override fun getApplicationContext(): Context = guestApplication ?: this
    override fun getPackageCodePath(): String = baseApk.absolutePath
    override fun getPackageResourcePath(): String = baseApk.absolutePath
    override fun getClassLoader(): ClassLoader = guestLoader
    override fun getAssets(): AssetManager = guestAssetManager
    override fun getResources(): Resources = guestResources
    override fun getApplicationInfo(): ApplicationInfo = guestAppInfo
    override fun getFilesDir(): File = guestFilesDir
    override fun getCacheDir(): File = guestCacheDir
    override fun getDataDir(): File = guestDataDir

    override fun getExternalFilesDir(type: String?): File {
        val base = if (isDataExposed && exposedDir != null) {
            File(exposedDir, "data/external")
        } else {
            File(containerDir, "external/files")
        }
        val dir = File(base, type ?: "").apply { mkdirs() }
        return dir
    }

    override fun getExternalFilesDirs(type: String?): Array<File> {
        return arrayOf(getExternalFilesDir(type))
    }

    override fun getObbDir(): File = guestObbDir
    override fun getObbDirs(): Array<File> = arrayOf(guestObbDir)

    override fun getExternalCacheDir(): File {
        val base = if (isDataExposed && exposedDir != null) {
            File(exposedDir, "data/cache")
        } else {
            File(containerDir, "external/cache")
        }
        return base.apply { mkdirs() }
    }

    override fun getDatabasePath(name: String): File {
        val dbDir = File(guestDataDir, "databases").apply { mkdirs() }
        return File(dbDir, name)
    }

    override fun getDir(name: String, mode: Int): File {
        val dir = File(guestDataDir, "app_$name").apply { mkdirs() }
        return dir
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
            Log.w("GuestContext", "Preventing bindService to Google service: $service")
            return false
        }
        return super.bindService(service, conn, flags)
    }

    override fun startService(service: android.content.Intent): android.content.ComponentName? {
        if (isGoogleService(service)) {
            Log.w("GuestContext", "Preventing startService to Google service: $service")
            return null
        }
        return super.startService(service)
    }

    override fun unbindService(conn: android.content.ServiceConnection) {
        try {
            super.unbindService(conn)
        } catch (e: IllegalArgumentException) {
            Log.w("GuestContext", "Swallowed unbindService exception: ${e.message}")
        }
    }

    override fun checkPermission(permission: String, pid: Int, uid: Int): Int =
        android.content.pm.PackageManager.PERMISSION_GRANTED

    override fun checkSelfPermission(permission: String): Int =
        android.content.pm.PackageManager.PERMISSION_GRANTED

    override fun checkCallingPermission(permission: String): Int =
        android.content.pm.PackageManager.PERMISSION_GRANTED

    override fun checkCallingOrSelfPermission(permission: String): Int =
        android.content.pm.PackageManager.PERMISSION_GRANTED

    override fun registerReceiver(receiver: android.content.BroadcastReceiver?, filter: android.content.IntentFilter?): android.content.Intent? {
        return try {
            if (android.os.Build.VERSION.SDK_INT >= 33) {
                super.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED)
            } else {
                super.registerReceiver(receiver, filter)
            }
        } catch (e: Throwable) {
            Log.w("GuestContext", "registerReceiver suppressed exception: ${e.message}")
            null
        }
    }

    override fun registerReceiver(receiver: android.content.BroadcastReceiver?, filter: android.content.IntentFilter?, flags: Int): android.content.Intent? {
        val patchedFlags = if (android.os.Build.VERSION.SDK_INT >= 33 && (flags and 0x6) == 0) {
            flags or Context.RECEIVER_EXPORTED
        } else {
            flags
        }
        return try {
            super.registerReceiver(receiver, filter, patchedFlags)
        } catch (e: Throwable) {
            Log.w("GuestContext", "registerReceiver with flags suppressed exception: ${e.message}")
            null
        }
    }

    override fun registerReceiver(receiver: android.content.BroadcastReceiver?, filter: android.content.IntentFilter?, broadcastPermission: String?, scheduler: android.os.Handler?): android.content.Intent? {
        return try {
            if (android.os.Build.VERSION.SDK_INT >= 33) {
                super.registerReceiver(receiver, filter, broadcastPermission, scheduler, Context.RECEIVER_EXPORTED)
            } else {
                super.registerReceiver(receiver, filter, broadcastPermission, scheduler)
            }
        } catch (e: Throwable) {
            Log.w("GuestContext", "registerReceiver with permission suppressed exception: ${e.message}")
            null
        }
    }

    override fun registerReceiver(receiver: android.content.BroadcastReceiver?, filter: android.content.IntentFilter?, broadcastPermission: String?, scheduler: android.os.Handler?, flags: Int): android.content.Intent? {
        val patchedFlags = if (android.os.Build.VERSION.SDK_INT >= 33 && (flags and 0x6) == 0) {
            flags or Context.RECEIVER_EXPORTED
        } else {
            flags
        }
        return try {
            super.registerReceiver(receiver, filter, broadcastPermission, scheduler, patchedFlags)
        } catch (e: Throwable) {
            Log.w("GuestContext", "registerReceiver with permission & flags suppressed exception: ${e.message}")
            null
        }
    }

    private var guestTheme: Resources.Theme? = null

    override fun getTheme(): Resources.Theme {
        if (guestTheme == null) {
            guestTheme = resources.newTheme().apply {
                try {
                    applyStyle(android.R.style.Theme_DeviceDefault, false)
                } catch (_: Throwable) {}
            }
        }
        return guestTheme!!
    }

    override fun setTheme(resid: Int) {
        super.setTheme(resid)
        try {
            theme.applyStyle(resid, true)
        } catch (_: Throwable) {}
    }

    private var guestLayoutInflater: LayoutInflater? = null
    private var guestWindowManager: WindowManager? = null

    override fun getSystemService(name: String): Any? {
        if (name == Context.LAYOUT_INFLATER_SERVICE) {
            if (guestLayoutInflater == null) {
                val inflater = super.getSystemService(name) as? LayoutInflater
                guestLayoutInflater = inflater?.cloneInContext(this)
            }
            return guestLayoutInflater
        }
        if (name == Context.WINDOW_SERVICE) {
            if (guestWindowManager == null) {
                val wm = super.getSystemService(name) as? WindowManager
                if (wm != null) {
                    guestWindowManager = GuestWindowManager(wm, baseContext.packageName)
                }
            }
            return guestWindowManager ?: super.getSystemService(name)
        }
        val s = super.getSystemService(name)
        if (name == Context.ALARM_SERVICE && s is android.app.AlarmManager) {
            try {
                val field = android.app.AlarmManager::class.java.getDeclaredField("mTargetSdkVersion")
                field.isAccessible = true
                field.setInt(s, 23)
            } catch (_: Throwable) {}
        }
        return s
    }
}

class GuestWindowManager(
    private val delegate: WindowManager,
    private val hostPackageName: String
) : WindowManager {
    override fun getDefaultDisplay(): android.view.Display = delegate.defaultDisplay
    override fun removeViewImmediate(view: View?) = delegate.removeViewImmediate(view)
    override fun addView(view: View?, params: ViewGroup.LayoutParams?) {
        if (params is WindowManager.LayoutParams) {
            params.packageName = hostPackageName
        }
        delegate.addView(view, params)
    }
    override fun updateViewLayout(view: View?, params: ViewGroup.LayoutParams?) {
        if (params is WindowManager.LayoutParams) {
            params.packageName = hostPackageName
        }
        delegate.updateViewLayout(view, params)
    }
    override fun removeView(view: View?) = delegate.removeView(view)
}
