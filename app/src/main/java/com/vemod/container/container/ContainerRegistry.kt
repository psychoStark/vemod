package com.vemod.container.container

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.BitmapFactory
import android.util.Log
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.graphics.drawable.IconCompat
import com.vemod.container.model.ContainerApp
import com.vemod.container.proxy.GuestProxyActivity
import java.io.File

object ContainerRegistry {
    private const val TAG = "ContainerRegistry"
    private const val PREFS_NAME = "vemod_slots"
    const val MAX_SLOTS = 10

    private fun getContainersDir(context: Context): File {
        return File(context.filesDir, "containers").apply { mkdirs() }
    }

    @Synchronized
    fun getInstalledApps(context: Context): List<ContainerApp> {
        val root = getContainersDir(context)
        val dirs = root.listFiles { f -> f.isDirectory && File(f, "base.apk").exists() } ?: return emptyList()

        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
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

            // Sync slot assignment from preferences
            var assignedSlot = -1
            for (i in 1..MAX_SLOTS) {
                if (prefs.getString("slot_$i", null) == app.packageName) {
                    assignedSlot = i
                    break
                }
            }
            if (app.slotIndex != assignedSlot) {
                app.slotIndex = assignedSlot
                ApkExtractor.saveMetadata(dir, app)
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

    fun getAppForSlot(context: Context, slotClassName: String): ContainerApp? {
        // e.g. "com.vemod.container.slot.AppSlot1" -> 1
        val match = Regex("""AppSlot(\d+)""").find(slotClassName) ?: return null
        val slotIndex = match.groupValues[1].toIntOrNull() ?: return null

        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val pkg = prefs.getString("slot_$slotIndex", null) ?: return null
        return getApp(context, pkg)
    }

    @Synchronized
    fun toggleAppDrawerSlot(context: Context, packageName: String): Int {
        val app = getApp(context, packageName) ?: return -1
        if (app.slotIndex > 0) {
            releaseSlot(context, packageName)
            return -1
        } else {
            return assignSlot(context, packageName)
        }
    }

    @Synchronized
    fun assignSlot(context: Context, packageName: String): Int {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val pm = context.packageManager

        // Check if already assigned
        for (i in 1..MAX_SLOTS) {
            if (prefs.getString("slot_$i", null) == packageName) {
                return i
            }
        }

        // Find next free slot
        var freeSlot = -1
        for (i in 1..MAX_SLOTS) {
            if (prefs.getString("slot_$i", null) == null) {
                freeSlot = i
                break
            }
        }

        if (freeSlot == -1) {
            Log.w(TAG, "No free App Drawer slots remaining (max $MAX_SLOTS)")
            return -1
        }

        // Enable component in PackageManager
        val componentName = ComponentName(context, "com.vemod.container.slot.AppSlot$freeSlot")
        try {
            pm.setComponentEnabledSetting(
                componentName,
                PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
                PackageManager.DONT_KILL_APP
            )
            prefs.edit().putString("slot_$freeSlot", packageName).apply()

            val app = getApp(context, packageName)
            if (app != null) {
                app.slotIndex = freeSlot
                ApkExtractor.saveMetadata(app.containerDir, app)
            }
            Log.i(TAG, "Assigned slot $freeSlot to $packageName and enabled $componentName")
        } catch (e: Throwable) {
            Log.e(TAG, "Failed to enable component $componentName", e)
            return -1
        }

        updateShortcuts(context)
        return freeSlot
    }

    @Synchronized
    fun releaseSlot(context: Context, packageName: String) {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val pm = context.packageManager

        for (i in 1..MAX_SLOTS) {
            if (prefs.getString("slot_$i", null) == packageName) {
                val componentName = ComponentName(context, "com.vemod.container.slot.AppSlot$i")
                try {
                    pm.setComponentEnabledSetting(
                        componentName,
                        PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                        PackageManager.DONT_KILL_APP
                    )
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to disable component $componentName", e)
                }
                prefs.edit().remove("slot_$i").apply()

                val app = getApp(context, packageName)
                if (app != null) {
                    app.slotIndex = -1
                    ApkExtractor.saveMetadata(app.containerDir, app)
                }
                Log.i(TAG, "Released slot $i for $packageName")
                break
            }
        }

        updateShortcuts(context)
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
        releaseSlot(context, packageName)
        try {
            ShortcutManagerCompat.removeDynamicShortcuts(context, listOf("dynamic_$packageName", "shortcut_$packageName"))
        } catch (_: Throwable) {}

        val dir = File(getContainersDir(context), packageName)
        val success = dir.deleteRecursively()
        Log.i(TAG, "Uninstalled $packageName: success=$success")
        return success
    }
}
