package com.vemod.container.virtual

import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ApplicationInfo
import android.content.res.AssetManager
import android.content.res.Resources
import android.util.Log
import java.io.File

class GuestContext(
    base: Context,
    private val guestPackageName: String,
    private val containerDir: File,
    private val baseApk: File,
    private val guestLoader: ClassLoader,
    private val preloadedAssets: AssetManager? = null,
    private val preloadedResources: Resources? = null
) : ContextWrapper(base) {

    private val guestFilesDir = File(containerDir, "data/files").apply { mkdirs() }
    private val guestCacheDir = File(containerDir, "data/cache").apply { mkdirs() }

    private val guestAssetManager: AssetManager by lazy {
        preloadedAssets ?: run {
            val am = AssetManager::class.java.getDeclaredConstructor().newInstance()
            val addAssetPath = AssetManager::class.java.getDeclaredMethod("addAssetPath", String::class.java)
            addAssetPath.invoke(am, baseApk.absolutePath)
            am
        }
    }

    private val guestResources: Resources by lazy {
        preloadedResources ?: run {
            val hostRes = base.resources
            Resources(guestAssetManager, hostRes.displayMetrics, hostRes.configuration)
        }
    }

    private val guestAppInfo: ApplicationInfo by lazy {
        ApplicationInfo(base.applicationInfo).apply {
            packageName = guestPackageName
            sourceDir = baseApk.absolutePath
            publicSourceDir = baseApk.absolutePath
            dataDir = containerDir.absolutePath
            nativeLibraryDir = File(containerDir, "proxy").absolutePath
        }
    }

    override fun getPackageName(): String = guestPackageName
    override fun getOpPackageName(): String = baseContext.opPackageName
    override fun getPackageCodePath(): String = baseApk.absolutePath
    override fun getPackageResourcePath(): String = baseApk.absolutePath
    override fun getClassLoader(): ClassLoader = guestLoader
    override fun getAssets(): AssetManager = guestAssetManager
    override fun getResources(): Resources = guestResources
    override fun getApplicationInfo(): ApplicationInfo = guestAppInfo
    override fun getFilesDir(): File = guestFilesDir
    override fun getCacheDir(): File = guestCacheDir

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

    override fun getExternalFilesDir(type: String?): File {
        val dir = File(containerDir, "external/files" + if (type != null) "/$type" else "").apply { mkdirs() }
        return dir
    }

    override fun getObbDir(): File {
        return File(containerDir, "external/obb").apply { mkdirs() }
    }
}
