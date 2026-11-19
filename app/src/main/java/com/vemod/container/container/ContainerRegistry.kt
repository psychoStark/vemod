package com.vemod.container.container

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.BitmapFactory
import android.os.Environment
import android.util.Log
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.graphics.drawable.IconCompat
import com.vemod.container.model.ContainerApp
import com.vemod.container.proxy.GuestProxyActivity
import java.io.File

object ContainerRegistry {
    private const val TAG = "ContainerRegistry"

    fun getContainersDir(context: Context): File {
        return File(context.filesDir, "containers").apply { mkdirs() }
    }

    @Synchronized
    fun getInstalledApps(context: Context): List<ContainerApp> {
        val root = getContainersDir(context)
        val dirs = root.listFiles { f -> f.isDirectory && File(f, "base.apk").exists() } ?: return emptyList()

        val apps = mutableListOf<ContainerApp>()

        for (dir in dirs) {
            var app = ApkExtractor.readMetadata(dir)
            if (app == null) {
                // If meta.json was missing, rebuild it from base.apk
                val baseApk = File(dir, "base.apk")
                if (!baseApk.exists()) continue
                try {
                    app = ApkExtractor.extractFromFile(context, baseApk)
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to re-index container: ${dir.name}", e)
                    continue
                }
            }
            apps.add(app)
        }

        return apps.sortedBy { it.appName.lowercase() }
    }

    fun getApp(context: Context, packageName: String): ContainerApp? {
        val dir = File(getContainersDir(context), packageName)
        if (!dir.exists()) return null
        return ApkExtractor.readMetadata(dir)
    }

    fun isAppInAppDrawer(context: Context, packageName: String): Boolean {
        return StubApkGenerator.isStubInstalled(context, packageName)
    }

    fun installToAppDrawer(context: Context, packageName: String) {
        val app = getApp(context, packageName) ?: return
        StubApkGenerator.installStub(context, app)
    }

    fun getExposedFolderRelativePath(context: Context, app: ContainerApp): String {
        val allApps = getInstalledApps(context)
        val sameNameCount = allApps.count { it.appName.equals(app.appName, ignoreCase = true) }
        val rawName = if (sameNameCount > 1) {
            "${app.appName} (${app.packageName})"
        } else {
            app.appName
        }
        return rawName.replace(Regex("[\\\\/:*?\"<>|]"), "_").trim()
    }

    fun getExposedDir(context: Context, app: ContainerApp): File {
        val sub = getExposedFolderRelativePath(context, app)
        val vemodBase = File(Environment.getExternalStorageDirectory(), "vemod")
        return File(vemodBase, sub)
    }

    @Synchronized
    fun setExposeAppData(context: Context, packageName: String, expose: Boolean): Boolean {
        val app = getApp(context, packageName) ?: return false
        if (app.isDataExposed == expose) return true

        val exposedDir = getExposedDir(context, app)
        val exposedData = File(exposedDir, "data")
        val exposedObb = File(exposedDir, "obb")
        val internalData = File(app.containerDir, "data")
        val internalObb = File(app.containerDir, "external/obb")

        try {
            if (expose) {
                exposedData.mkdirs()
                File(exposedData, "files").mkdirs()
                File(exposedData, "cache").mkdirs()
                exposedObb.mkdirs()

                if (internalData.exists()) {
                    copyDirectoryContents(internalData, exposedData)
                }
                if (internalObb.exists()) {
                    copyDirectoryContents(internalObb, exposedObb)
                }
            } else {
                internalData.mkdirs()
                internalObb.mkdirs()
                if (exposedData.exists()) {
                    copyDirectoryContents(exposedData, internalData)
                }
                if (exposedObb.exists()) {
                    copyDirectoryContents(exposedObb, internalObb)
                }
            }

            app.isDataExposed = expose
            ApkExtractor.saveMetadata(app.containerDir, app)
            updateShortcuts(context)
            Log.i(TAG, "setExposeAppData for ${app.packageName}: expose=$expose, dir=${exposedDir.absolutePath}")
            return true
        } catch (e: Throwable) {
            Log.e(TAG, "Failed to setExposeAppData for ${app.packageName}", e)
            return false
        }
    }

    private fun copyDirectoryContents(source: File, target: File) {
        if (!source.exists() || !source.isDirectory) return
        target.mkdirs()
        source.listFiles()?.forEach { file ->
            val dest = File(target, file.name)
            if (file.isDirectory) {
                copyDirectoryContents(file, dest)
            } else {
                try {
                    if (!dest.exists() || file.lastModified() > dest.lastModified()) {
                        file.copyTo(dest, overwrite = true)
                    }
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to copy file ${file.absolutePath} to ${dest.absolutePath}", e)
                }
            }
        }
    }

    fun pinShortcut(context: Context, app: ContainerApp): Boolean {
        if (!ShortcutManagerCompat.isRequestPinShortcutSupported(context)) {
            Log.w(TAG, "Pinned shortcuts not supported on this launcher")
            return false
        }

        val launchIntent = Intent(context, GuestProxyActivity::class.java).apply {
            action = Intent.ACTION_VIEW
            putExtra(GuestProxyActivity.EXTRA_CONTAINER_DIR, app.containerDir.absolutePath)
            putExtra(GuestProxyActivity.EXTRA_BASE_APK, app.baseApk.absolutePath)
            putExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME, app.packageName)
            putExtra(GuestProxyActivity.EXTRA_LAUNCH_ACTIVITY, app.launchActivity)
            putExtra(GuestProxyActivity.EXTRA_SCREEN_ORIENTATION, app.screenOrientation)
            putExtra(GuestProxyActivity.EXTRA_IS_DATA_EXPOSED, app.isDataExposed)
            if (app.isDataExposed) {
                val exp = getExposedDir(context, app)
                putExtra(GuestProxyActivity.EXTRA_EXPOSED_DIR, exp.absolutePath)
            }
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }

        val iconCompat = if (app.iconFile != null && app.iconFile.exists()) {
            val bitmap = BitmapFactory.decodeFile(app.iconFile.absolutePath)
            if (bitmap != null) {
                IconCompat.createWithBitmap(bitmap)
            } else {
                IconCompat.createWithResource(context, android.R.drawable.sym_def_app_icon)
            }
        } else {
            IconCompat.createWithResource(context, android.R.drawable.sym_def_app_icon)
        }

        val shortcutInfo = ShortcutInfoCompat.Builder(context, "shortcut_${app.packageName}")
            .setShortLabel(app.appName)
            .setLongLabel(app.appName)
            .setIcon(iconCompat)
            .setIntent(launchIntent)
            .build()

        return ShortcutManagerCompat.requestPinShortcut(context, shortcutInfo, null)
    }

    fun updateShortcuts(context: Context) {
        try {
            val apps = getInstalledApps(context).take(4)
            val shortcuts = apps.map { app ->
                val launchIntent = Intent(context, GuestProxyActivity::class.java).apply {
                    action = Intent.ACTION_VIEW
                    putExtra(GuestProxyActivity.EXTRA_CONTAINER_DIR, app.containerDir.absolutePath)
                    putExtra(GuestProxyActivity.EXTRA_BASE_APK, app.baseApk.absolutePath)
                    putExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME, app.packageName)
                    putExtra(GuestProxyActivity.EXTRA_LAUNCH_ACTIVITY, app.launchActivity)
                    putExtra(GuestProxyActivity.EXTRA_SCREEN_ORIENTATION, app.screenOrientation)
                    putExtra(GuestProxyActivity.EXTRA_IS_DATA_EXPOSED, app.isDataExposed)
                    if (app.isDataExposed) {
                        val exp = getExposedDir(context, app)
                        putExtra(GuestProxyActivity.EXTRA_EXPOSED_DIR, exp.absolutePath)
                    }
                    flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
                }

                val iconCompat = if (app.iconFile != null && app.iconFile.exists()) {
                    val bitmap = BitmapFactory.decodeFile(app.iconFile.absolutePath)
                    if (bitmap != null) IconCompat.createWithBitmap(bitmap)
                    else IconCompat.createWithResource(context, android.R.drawable.sym_def_app_icon)
                } else {
                    IconCompat.createWithResource(context, android.R.drawable.sym_def_app_icon)
                }

                ShortcutInfoCompat.Builder(context, "dynamic_${app.packageName}")
                    .setShortLabel(app.appName)
                    .setLongLabel(app.appName)
                    .setIcon(iconCompat)
                    .setIntent(launchIntent)
                    .build()
            }
            ShortcutManagerCompat.setDynamicShortcuts(context, shortcuts)
        } catch (e: Throwable) {
            Log.w(TAG, "Failed to update dynamic shortcuts", e)
        }
    }

    @Synchronized
    fun uninstallApp(context: Context, packageName: String): Boolean {
        StubApkGenerator.uninstallStub(context, packageName)
        try {
            ShortcutManagerCompat.removeDynamicShortcuts(context, listOf("dynamic_$packageName", "shortcut_$packageName"))
        } catch (_: Throwable) {}

        // Release any running process slots associated with this container
        try {
            val prefs = context.getSharedPreferences("vemod_guest_processes", Context.MODE_PRIVATE)
            val editor = prefs.edit()
            for (i in 1..com.vemod.container.proxy.GuestProcessManager.NUM_PROCESSES) {
                if (prefs.getString("proc_$i", null) == packageName) {
                    editor.remove("proc_$i")
                }
            }
            editor.apply()
        } catch (_: Throwable) {}

        val dir = File(getContainersDir(context), packageName)
        val success = deleteRecursivelyWithPerms(dir)
        Log.i(TAG, "Uninstalled $packageName: success=$success")
        return success
    }

    fun recordAppLaunch(context: Context, packageName: String) {
        try {
            val prefs = context.getSharedPreferences("vemod_settings", Context.MODE_PRIVATE)
            prefs.edit().putLong("last_launch_$packageName", System.currentTimeMillis()).apply()
        } catch (e: Throwable) {
            Log.w(TAG, "Failed to record app launch: $packageName", e)
        }
    }

    fun getLastLaunchTime(context: Context, packageName: String): Long {
        return try {
            val prefs = context.getSharedPreferences("vemod_settings", Context.MODE_PRIVATE)
            prefs.getLong("last_launch_$packageName", 0L)
        } catch (_: Throwable) {
            0L
        }
    }

    fun isPipEnabledForApp(context: Context, packageName: String): Boolean {
        val prefs = context.getSharedPreferences("vemod_settings", Context.MODE_PRIVATE)
        val key = "pip_enabled_$packageName"
        return if (prefs.contains(key)) {
            prefs.getBoolean(key, false)
        } else {
            prefs.getBoolean("pip_enabled", false)
        }
    }

    fun setPipEnabledForApp(context: Context, packageName: String, enabled: Boolean) {
        val prefs = context.getSharedPreferences("vemod_settings", Context.MODE_PRIVATE)
        prefs.edit().putBoolean("pip_enabled_$packageName", enabled).apply()
    }

    private fun deleteRecursivelyWithPerms(file: File): Boolean {
        if (!file.exists()) return true
        file.walkBottomUp().forEach { f ->
            try {
                f.setWritable(true, false)
            } catch (_: Throwable) {}
            f.delete()
        }
        return !file.exists()
    }
}
