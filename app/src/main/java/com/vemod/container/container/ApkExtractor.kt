package com.vemod.container.container

import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.drawable.BitmapDrawable
import android.net.Uri
import android.util.Log
import com.vemod.container.model.ContainerApp
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipInputStream

object ApkExtractor {
    private const val TAG = "ApkExtractor"

    fun extract(context: Context, apkUri: Uri): ContainerApp {
        val tempApk = File(context.cacheDir, "import_temp_${System.currentTimeMillis()}.apk")
        context.contentResolver.openInputStream(apkUri).use { input ->
            requireNotNull(input) { "Cannot open APK URI: $apkUri" }
            FileOutputStream(tempApk).use { output ->
                input.copyTo(output)
            }
        }
        return extractFromFile(context, tempApk).also {
            tempApk.delete()
        }
    }

    fun extractFromFile(context: Context, apkFile: File): ContainerApp {
        val pm = context.packageManager
        val pkgInfo = pm.getPackageArchiveInfo(
            apkFile.absolutePath,
            PackageManager.GET_ACTIVITIES
        ) ?: error("Failed to parse APK archive: ${apkFile.absolutePath}")

        val packageName = pkgInfo.packageName
        val launchActivityInfo = pkgInfo.activities?.firstOrNull()
        val launchActivity = launchActivityInfo?.name
            ?: error("No launch activity found in APK: $packageName")
        val screenOrientation = launchActivityInfo.screenOrientation

        val appInfo = pkgInfo.applicationInfo
        val appName = try {
            if (appInfo != null) {
                appInfo.sourceDir = apkFile.absolutePath
                appInfo.publicSourceDir = apkFile.absolutePath
                appInfo.loadLabel(pm).toString()
            } else {
                packageName
            }
        } catch (_: Throwable) {
            packageName
        }

        val versionName = pkgInfo.versionName ?: "1.0"
        val versionCode = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P) {
            pkgInfo.longVersionCode
        } else {
            @Suppress("DEPRECATION")
            pkgInfo.versionCode.toLong()
        }

        val containerDir = File(context.filesDir, "containers/$packageName").apply { mkdirs() }
        val lib32Dir = File(containerDir, "lib32").apply { mkdirs() }
        val proxyDir = File(containerDir, "proxy").apply { mkdirs() }
        val dataFilesDir = File(containerDir, "data/files").apply { mkdirs() }
        val dataCacheDir = File(containerDir, "data/cache").apply { mkdirs() }
        val baseApk = File(containerDir, "base.apk")

        // Copy APK to base.apk and make read-only for Android 14+ ART security
        if (apkFile.canonicalPath != baseApk.canonicalPath) {
            if (baseApk.exists()) {
                baseApk.setWritable(true)
            }
            apkFile.copyTo(baseApk, overwrite = true)
            baseApk.setReadOnly()
        }

        // Extract high-resolution app icon
        val iconFile = File(containerDir, "icon.png")
        try {
            if (appInfo != null) {
                appInfo.sourceDir = baseApk.absolutePath
                appInfo.publicSourceDir = baseApk.absolutePath
                val drawable = appInfo.loadIcon(pm)
                if (drawable != null) {
                    val bitmap = if (drawable is BitmapDrawable && drawable.bitmap != null) {
                        drawable.bitmap
                    } else {
                        val w = if (drawable.intrinsicWidth > 0) drawable.intrinsicWidth else 192
                        val h = if (drawable.intrinsicHeight > 0) drawable.intrinsicHeight else 192
                        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
                        val canvas = Canvas(bmp)
                        drawable.setBounds(0, 0, canvas.width, canvas.height)
                        drawable.draw(canvas)
                        bmp
                    }
                    FileOutputStream(iconFile).use { fos ->
                        bitmap.compress(Bitmap.CompressFormat.PNG, 100, fos)
                    }
                }
            }
        } catch (e: Throwable) {
            Log.w(TAG, "Failed to extract app icon", e)
        }

        // Find the host app's compiled libproxy.so
        val hostProxyTemplate = File(context.applicationInfo.nativeLibraryDir, "libproxy.so")
        Log.i(TAG, "Host proxy template: ${hostProxyTemplate.absolutePath} (exists: ${hostProxyTemplate.exists()})")

        val extractedLibs = mutableListOf<String>()

        // Detect available 32-bit ARM ABI: prefer armeabi-v7a, fallback to armeabi
        var hasV7a = false
        var hasV5 = false
        try {
            ZipFile(baseApk).use { zf ->
                val entries = zf.entries()
                while (entries.hasMoreElements()) {
                    val name = entries.nextElement().name
                    if (name.startsWith("lib/armeabi-v7a/") && name.endsWith(".so")) hasV7a = true
                    if (name.startsWith("lib/armeabi/") && name.endsWith(".so")) hasV5 = true
                }
            }
        } catch (e: Throwable) {
            Log.w(TAG, "Failed to inspect APK zip entries", e)
        }

        val selectedAbiPrefix = when {
            hasV7a -> "lib/armeabi-v7a/"
            hasV5 -> "lib/armeabi/"
            else -> "lib/armeabi-v7a/"
        }
        Log.i(TAG, "Selected ABI prefix: $selectedAbiPrefix (v7a=$hasV7a, armeabi=$hasV5)")

        // Unpack libraries from chosen ABI
        ZipInputStream(FileInputStream(baseApk)).use { zis ->
            var entry: ZipEntry? = zis.nextEntry
            while (entry != null) {
                val name = entry.name
                if (name.startsWith(selectedAbiPrefix) && name.endsWith(".so")) {
                    val libName = name.substringAfterLast("/")
                    val targetLib = File(lib32Dir, libName)
                    FileOutputStream(targetLib).use { fos ->
                        zis.copyTo(fos)
                    }
                    extractedLibs.add(libName)
                    Log.i(TAG, "Extracted 32-bit library: $libName (${targetLib.length()} bytes)")

                    // Drop 64-bit libproxy.so template into proxyDir under the same library name
                    val proxyTarget = File(proxyDir, libName)
                    if (hostProxyTemplate.exists()) {
                        hostProxyTemplate.copyTo(proxyTarget, overwrite = true)
                        Log.i(TAG, "Created proxy stub: ${proxyTarget.absolutePath}")
                    } else {
                        Log.w(TAG, "Host proxy template missing at ${hostProxyTemplate.absolutePath}")
                    }
                }
                entry = zis.nextEntry
            }
        }

        val containerApp = ContainerApp(
            packageName = packageName,
            appName = appName,
            launchActivity = launchActivity,
            versionName = versionName,
            versionCode = versionCode,
            containerDir = containerDir,
            baseApk = baseApk,
            lib32Dir = lib32Dir,
            proxyDir = proxyDir,
            iconFile = if (iconFile.exists()) iconFile else null,
            nativeLibraries = extractedLibs,
            installedAt = System.currentTimeMillis(),
            screenOrientation = screenOrientation
        )

        // Save metadata JSON
        saveMetadata(containerDir, containerApp)

        return containerApp
    }

    fun saveMetadata(containerDir: File, app: ContainerApp) {
        try {
            val json = JSONObject().apply {
                put("packageName", app.packageName)
                put("appName", app.appName)
                put("launchActivity", app.launchActivity)
                put("versionName", app.versionName)
                put("versionCode", app.versionCode)
                put("slotIndex", app.slotIndex)
                put("installedAt", app.installedAt)
                put("nativeLibraries", JSONArray(app.nativeLibraries))
                put("screenOrientation", app.screenOrientation)
            }
            File(containerDir, "meta.json").writeText(json.toString(2))
        } catch (e: Throwable) {
            Log.w(TAG, "Failed to save metadata", e)
        }
    }

    fun readMetadata(containerDir: File): ContainerApp? {
        val baseApk = File(containerDir, "base.apk")
        if (!baseApk.exists()) return null

        val metaFile = File(containerDir, "meta.json")
        val iconFile = File(containerDir, "icon.png").let { if (it.exists()) it else null }
        val lib32Dir = File(containerDir, "lib32")
        val proxyDir = File(containerDir, "proxy")

        if (metaFile.exists()) {
            try {
                val json = JSONObject(metaFile.readText())
                val nativeLibs = mutableListOf<String>()
                val libsArray = json.optJSONArray("nativeLibraries")
                if (libsArray != null) {
                    for (i in 0 until libsArray.length()) {
                        nativeLibs.add(libsArray.getString(i))
                    }
                } else {
                    lib32Dir.listFiles()?.forEach { nativeLibs.add(it.name) }
                }

                val screenOrientation = if (json.has("screenOrientation")) {
                    json.getInt("screenOrientation")
                } else {
                    try {
                        val pm = com.vemod.container.VemodApp.instance?.packageManager
                        val archiveInfo = pm?.getPackageArchiveInfo(baseApk.absolutePath, PackageManager.GET_ACTIVITIES)
                        archiveInfo?.activities?.firstOrNull()?.screenOrientation ?: android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                    } catch (_: Throwable) {
                        android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                    }
                }

                return ContainerApp(
                    packageName = json.getString("packageName"),
                    appName = json.optString("appName", json.getString("packageName")),
                    launchActivity = json.getString("launchActivity"),
                    versionName = json.optString("versionName", "1.0"),
                    versionCode = json.optLong("versionCode", 1L),
                    containerDir = containerDir,
                    baseApk = baseApk,
                    lib32Dir = lib32Dir,
                    proxyDir = proxyDir,
                    iconFile = iconFile,
                    nativeLibraries = nativeLibs,
                    slotIndex = json.optInt("slotIndex", -1),
                    installedAt = json.optLong("installedAt", System.currentTimeMillis()),
                    screenOrientation = screenOrientation
                )
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to parse meta.json in ${containerDir.name}", e)
            }
        }

        // Fallback: derive from base.apk
        return null
    }
}
