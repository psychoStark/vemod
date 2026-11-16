package com.vemod.container.container

import android.content.Context
import android.content.pm.PackageManager
import android.content.res.AssetManager
import android.content.res.Resources
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.net.Uri
import android.util.DisplayMetrics
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
        val tempDir = File(context.cacheDir, "import_temp_${System.currentTimeMillis()}").apply { mkdirs() }
        val tempFile = File(tempDir, "archive.tmp")
        try {
            context.contentResolver.openInputStream(apkUri).use { input ->
                requireNotNull(input) { "Cannot open APK URI: $apkUri" }
                FileOutputStream(tempFile).use { output ->
                    input.copyTo(output)
                }
            }
            return extractArchiveOrApk(context, tempFile, tempDir)
        } finally {
            tempDir.deleteRecursively()
        }
    }

    fun extractFromFile(context: Context, file: File): ContainerApp {
        val tempDir = File(context.cacheDir, "import_temp_${System.currentTimeMillis()}").apply { mkdirs() }
        try {
            return extractArchiveOrApk(context, file, tempDir)
        } finally {
            tempDir.deleteRecursively()
        }
    }

    private fun extractArchiveOrApk(context: Context, file: File, tempDir: File): ContainerApp {
        var isDirectApk = false
        try {
            ZipFile(file).use { zf ->
                if (zf.getEntry("AndroidManifest.xml") != null) {
                    isDirectApk = true
                }
            }
        } catch (_: Throwable) {}

        if (isDirectApk) {
            return extractFromSingleApk(context, file)
        }

        // Unpack multi-APK / XAPK / APKS bundle
        Log.i(TAG, "extractArchiveOrApk: unpacking bundle ${file.name}")
        val unpackedDir = File(tempDir, "unpacked").apply { mkdirs() }
        val obbFiles = mutableListOf<File>()
        val apkCandidates = mutableListOf<File>()

        ZipFile(file).use { zf ->
            val entries = zf.entries()
            while (entries.hasMoreElements()) {
                val entry = entries.nextElement()
                if (entry.isDirectory) continue
                val entryName = entry.name
                val entryLower = entryName.lowercase()
                if (entryLower.endsWith(".apk")) {
                    val dest = File(unpackedDir, File(entryName).name)
                    dest.parentFile?.mkdirs()
                    zf.getInputStream(entry).use { inStream ->
                        FileOutputStream(dest).use { outStream ->
                            inStream.copyTo(outStream)
                        }
                    }
                    apkCandidates.add(dest)
                } else if (entryLower.endsWith(".obb")) {
                    val dest = File(unpackedDir, File(entryName).name)
                    dest.parentFile?.mkdirs()
                    zf.getInputStream(entry).use { inStream ->
                        FileOutputStream(dest).use { outStream ->
                            inStream.copyTo(outStream)
                        }
                    }
                    obbFiles.add(dest)
                }
            }
        }

        require(apkCandidates.isNotEmpty()) { "No .apk files found inside archive: ${file.name}" }

        // Find best APK: prefer base.apk, or the one with launcher activity, or largest
        val bestApk = apkCandidates.find { it.name.equals("base.apk", ignoreCase = true) }
            ?: apkCandidates.maxByOrNull { it.length() }!!

        Log.i(TAG, "Selected main APK from bundle: ${bestApk.name} (${bestApk.length()} bytes)")
        val app = extractFromSingleApk(context, bestApk)

        // Install bundled OBBs
        if (obbFiles.isNotEmpty()) {
            val targetObbDir = File(app.containerDir, "external/obb").apply { mkdirs() }
            for (obb in obbFiles) {
                val destObb = File(targetObbDir, obb.name)
                obb.copyTo(destObb, overwrite = true)
                Log.i(TAG, "Placed bundled OBB: ${destObb.absolutePath} (${destObb.length()} bytes)")
                if (app.isDataExposed) {
                    try {
                        val exposedObb = File(ContainerRegistry.getExposedDir(context, app), "obb/${obb.name}")
                        exposedObb.parentFile?.mkdirs()
                        obb.copyTo(exposedObb, overwrite = true)
                    } catch (e: Throwable) {
                        Log.w(TAG, "Failed to copy OBB to exposed storage", e)
                    }
                }
            }
        }

        return app
    }

    private fun extractFromSingleApk(context: Context, apkFile: File): ContainerApp {
        val pm = context.packageManager
        val pkgInfo = pm.getPackageArchiveInfo(
            apkFile.absolutePath,
            PackageManager.GET_ACTIVITIES
        ) ?: error("Failed to parse APK archive: ${apkFile.absolutePath}")

        val manifestInfo = getManifestInfo(apkFile)
        val packageName = if (manifestInfo.packageName.isNotEmpty()) manifestInfo.packageName else pkgInfo.packageName
        val activities = pkgInfo.activities
        val manifestLauncher = manifestInfo.launcherActivity
        var launchActivityInfo = if (manifestLauncher != null) {
            activities?.find { it.name == manifestLauncher }
        } else null

        if (launchActivityInfo == null) {
            launchActivityInfo = activities?.find { it.name.contains("MainActivity") }
                ?: activities?.firstOrNull()
        }

        // If UnityPlayerActivity is present in the APK, prefer it over UnityPlayerProxyActivity or UnityPlayerNativeActivity
        val unityPlayerActivity = activities?.find { it.name == "com.unity3d.player.UnityPlayerActivity" }
            ?: if (manifestInfo.activities.containsKey("com.unity3d.player.UnityPlayerActivity")) {
                activities?.firstOrNull()
            } else null
        if (unityPlayerActivity != null &&
            (launchActivityInfo?.name?.endsWith("UnityPlayerProxyActivity") == true ||
             launchActivityInfo?.name?.endsWith("UnityPlayerNativeActivity") == true ||
             manifestLauncher?.endsWith("UnityPlayerProxyActivity") == true ||
             manifestLauncher?.endsWith("UnityPlayerNativeActivity") == true)) {
            Log.i(TAG, "extractFromFile: preferring UnityPlayerActivity over ${launchActivityInfo?.name ?: manifestLauncher}")
            launchActivityInfo = unityPlayerActivity
        }
        val launchActivity = if (unityPlayerActivity != null &&
            (manifestLauncher?.endsWith("UnityPlayerProxyActivity") == true ||
             manifestLauncher?.endsWith("UnityPlayerNativeActivity") == true)) {
            "com.unity3d.player.UnityPlayerActivity"
        } else {
            launchActivityInfo?.name ?: manifestLauncher
                ?: error("No launch activity found in APK: $packageName")
        }
        val screenOrientation = launchActivityInfo?.screenOrientation
            ?: manifestInfo.activities[launchActivity]?.screenOrientation
            ?: android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED

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
            var iconDrawable: Drawable? = null
            // Method 1: Load via AssetManager with baseApk asset path
            try {
                val am = AssetManager::class.java.newInstance()
                val addAssetPath = AssetManager::class.java.getMethod("addAssetPath", String::class.java)
                addAssetPath.invoke(am, baseApk.absolutePath)
                val res = Resources(am, context.resources.displayMetrics, context.resources.configuration)
                if (appInfo != null && appInfo.icon != 0) {
                    iconDrawable = res.getDrawableForDensity(appInfo.icon, DisplayMetrics.DENSITY_XXXHIGH, null)
                        ?: res.getDrawable(appInfo.icon, null)
                }
            } catch (e: Throwable) {
                Log.w(TAG, "AssetManager icon extraction fallback", e)
            }

            // Method 2: Fallback to appInfo.loadIcon(pm)
            if (iconDrawable == null && appInfo != null) {
                appInfo.sourceDir = baseApk.absolutePath
                appInfo.publicSourceDir = baseApk.absolutePath
                iconDrawable = appInfo.loadIcon(pm)
            }

            // Method 3: Fallback by searching common icon entries directly in APK zip
            if (iconDrawable == null) {
                ZipFile(baseApk).use { zf ->
                    val candidates = listOf(
                        "res/drawable-xxxhdpi/ic_launcher.png",
                        "res/drawable-xxhdpi/ic_launcher.png",
                        "res/drawable-xhdpi/ic_launcher.png",
                        "res/drawable-hdpi/ic_launcher.png",
                        "res/drawable/ic_launcher.png",
                        "res/drawable-xxxhdpi-v4/icon.png",
                        "res/drawable-xxhdpi-v4/icon.png",
                        "res/drawable-xhdpi-v4/icon.png",
                        "res/drawable-hdpi-v4/icon.png",
                        "res/drawable/icon.png"
                    )
                    for (c in candidates) {
                        val entry = zf.getEntry(c)
                        if (entry != null) {
                            val bmp = BitmapFactory.decodeStream(zf.getInputStream(entry))
                            if (bmp != null) {
                                iconDrawable = BitmapDrawable(context.resources, bmp)
                                break
                            }
                        }
                    }
                }
            }

            if (iconDrawable != null) {
                val bitmap = if (iconDrawable is BitmapDrawable && iconDrawable.bitmap != null) {
                    iconDrawable.bitmap
                } else {
                    val w = if (iconDrawable.intrinsicWidth > 0) iconDrawable.intrinsicWidth else 192
                    val h = if (iconDrawable.intrinsicHeight > 0) iconDrawable.intrinsicHeight else 192
                    val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
                    val canvas = Canvas(bmp)
                    iconDrawable.setBounds(0, 0, canvas.width, canvas.height)
                    iconDrawable.draw(canvas)
                    bmp
                }
                FileOutputStream(iconFile).use { fos ->
                    bitmap.compress(Bitmap.CompressFormat.PNG, 100, fos)
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
            screenOrientation = screenOrientation,
            applicationClass = manifestInfo.applicationClass
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
                put("isDataExposed", app.isDataExposed)
                put("applicationClass", app.applicationClass ?: "")
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
                        val manifestInfo = getManifestInfo(baseApk)
                        manifestInfo.activities.values.firstOrNull()?.screenOrientation
                            ?: android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                    } catch (_: Throwable) {
                        android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                    }
                }

                var launchActivity = json.getString("launchActivity")
                if (launchActivity.endsWith("UnityPlayerProxyActivity") ||
                    launchActivity.endsWith("UnityPlayerNativeActivity")) {
                    try {
                        val manifestInfo = getManifestInfo(baseApk)
                        if (manifestInfo.activities.containsKey("com.unity3d.player.UnityPlayerActivity")) {
                            Log.i(TAG, "readMetadata: auto-aliasing legacy launchActivity $launchActivity -> com.unity3d.player.UnityPlayerActivity")
                            launchActivity = "com.unity3d.player.UnityPlayerActivity"
                        }
                    } catch (_: Throwable) {}
                }

                val appClass = if (json.has("applicationClass") && json.optString("applicationClass").isNotEmpty()) {
                    json.getString("applicationClass")
                } else {
                    getManifestInfo(baseApk).applicationClass
                }

                return ContainerApp(
                    packageName = json.getString("packageName"),
                    appName = json.optString("appName", json.getString("packageName")),
                    launchActivity = launchActivity,
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
                    screenOrientation = screenOrientation,
                    isDataExposed = json.optBoolean("isDataExposed", false),
                    applicationClass = appClass
                )
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to parse meta.json in ${containerDir.name}", e)
            }
        }

        // Fallback: derive from base.apk
        return null
    }

    data class ActivityMeta(
        val name: String,
        val screenOrientation: Int = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED,
        val theme: Int = 0
    )

    data class ComponentMeta(
        val name: String,
        val processName: String? = null,
        val authority: String? = null,
        val exported: Boolean = false
    )

    data class ApkManifestInfo(
        val packageName: String,
        val versionName: String? = "1.0",
        val versionCode: Int? = 1,
        val applicationClass: String? = null,
        val applicationTheme: Int = 0,
        val targetSdkVersion: Int = 28,
        val launcherActivity: String? = null,
        val activities: Map<String, ActivityMeta> = emptyMap(),
        val providers: List<ComponentMeta> = emptyList(),
        val services: List<ComponentMeta> = emptyList(),
        val receivers: List<ComponentMeta> = emptyList(),
        val metaData: android.os.Bundle = android.os.Bundle()
    )

    private val manifestCache = java.util.concurrent.ConcurrentHashMap<String, ApkManifestInfo>()

    fun getManifestInfo(apkFile: File): ApkManifestInfo {
        val cacheKey = "${apkFile.absolutePath}:${apkFile.lastModified()}"
        return manifestCache.getOrPut(cacheKey) {
            parseManifest(apkFile)
        }
    }

    fun parseManifest(apkFile: File): ApkManifestInfo {
        var pkg = ""
        var verName: String? = "1.0"
        var verCode: Int? = 1
        var appClass: String? = null
        var appTheme = 0
        var targetSdk = 28
        var launcherActivity: String? = null
        val activities = mutableMapOf<String, ActivityMeta>()
        val providers = mutableListOf<ComponentMeta>()
        val services = mutableListOf<ComponentMeta>()
        val receivers = mutableListOf<ComponentMeta>()
        val metaData = android.os.Bundle()

        try {
            val assetManagerClass = Class.forName("android.content.res.AssetManager")
            val assetManager = assetManagerClass.newInstance() as android.content.res.AssetManager
            val addAssetPath = assetManagerClass.getMethod("addAssetPath", String::class.java)
            val cookie = addAssetPath.invoke(assetManager, apkFile.absolutePath) as Int
            if (cookie != 0) {
                val res = Resources(assetManager, DisplayMetrics().apply { setToDefaults() }, null)
                val parser = assetManager.openXmlResourceParser(cookie, "AndroidManifest.xml")
                var eventType = parser.eventType
                var curActivityName: String? = null
                var curOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                var curTheme = 0
                var hasMain = false
                var hasLauncher = false

                while (eventType != org.xmlpull.v1.XmlPullParser.END_DOCUMENT) {
                    if (eventType == org.xmlpull.v1.XmlPullParser.START_TAG) {
                        when (parser.name) {
                            "manifest" -> {
                                for (i in 0 until parser.attributeCount) {
                                    val attrName = parser.getAttributeName(i)
                                    if (attrName == "package") {
                                        pkg = parser.getAttributeValue(i)
                                    } else if (attrName == "versionName") {
                                        verName = parser.getAttributeValue(i)
                                    } else if (attrName == "versionCode") {
                                        verCode = parser.getAttributeIntValue(i, 1)
                                    }
                                }
                            }
                            "uses-sdk" -> {
                                for (i in 0 until parser.attributeCount) {
                                    if (parser.getAttributeName(i) == "targetSdkVersion") {
                                        val v = parser.getAttributeIntValue(i, 28)
                                        if (v > 0) targetSdk = v
                                    }
                                }
                            }
                            "application" -> {
                                for (i in 0 until parser.attributeCount) {
                                    val attrName = parser.getAttributeName(i)
                                    if (attrName == "name") {
                                        appClass = parser.getAttributeValue(i)
                                    } else if (attrName == "theme") {
                                        appTheme = parser.getAttributeResourceValue(i, 0)
                                    }
                                }
                            }
                            "activity" -> {
                                curActivityName = null
                                curOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                                curTheme = 0
                                hasMain = false
                                hasLauncher = false
                                for (i in 0 until parser.attributeCount) {
                                    val attrName = parser.getAttributeName(i)
                                    if (attrName == "name") {
                                        curActivityName = parser.getAttributeValue(i)
                                    } else if (attrName == "screenOrientation") {
                                        curOrientation = parser.getAttributeIntValue(i, android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED)
                                    } else if (attrName == "theme") {
                                        curTheme = parser.getAttributeResourceValue(i, 0)
                                    }
                                }
                            }
                            "provider" -> {
                                var pName: String? = null
                                var pAuth: String? = null
                                var pProc: String? = null
                                var pExp = false
                                for (i in 0 until parser.attributeCount) {
                                    val attrName = parser.getAttributeName(i)
                                    if (attrName == "name") {
                                        pName = parser.getAttributeValue(i)
                                    } else if (attrName == "authorities") {
                                        pAuth = parser.getAttributeValue(i)
                                    } else if (attrName == "process") {
                                        pProc = parser.getAttributeValue(i)
                                    } else if (attrName == "exported") {
                                        pExp = parser.getAttributeBooleanValue(i, false)
                                    }
                                }
                                if (pName != null) {
                                    val full = if (pName.startsWith(".")) pkg + pName
                                        else if (!pName.contains(".")) "$pkg.$pName"
                                        else pName
                                    providers.add(ComponentMeta(full, pProc, pAuth, pExp))
                                }
                            }
                            "service" -> {
                                var sName: String? = null
                                var sProc: String? = null
                                for (i in 0 until parser.attributeCount) {
                                    val attrName = parser.getAttributeName(i)
                                    if (attrName == "name") {
                                        sName = parser.getAttributeValue(i)
                                    } else if (attrName == "process") {
                                        sProc = parser.getAttributeValue(i)
                                    }
                                }
                                if (sName != null) {
                                    val full = if (sName.startsWith(".")) pkg + sName
                                        else if (!sName.contains(".")) "$pkg.$sName"
                                        else sName
                                    services.add(ComponentMeta(full, sProc))
                                }
                            }
                            "receiver" -> {
                                var rName: String? = null
                                var rProc: String? = null
                                for (i in 0 until parser.attributeCount) {
                                    val attrName = parser.getAttributeName(i)
                                    if (attrName == "name") {
                                        rName = parser.getAttributeValue(i)
                                    } else if (attrName == "process") {
                                        rProc = parser.getAttributeValue(i)
                                    }
                                }
                                if (rName != null) {
                                    val full = if (rName.startsWith(".")) pkg + rName
                                        else if (!rName.contains(".")) "$pkg.$rName"
                                        else rName
                                    receivers.add(ComponentMeta(full, rProc))
                                }
                            }
                            "action" -> {
                                for (i in 0 until parser.attributeCount) {
                                    if (parser.getAttributeName(i) == "name" &&
                                        parser.getAttributeValue(i) == "android.intent.action.MAIN") {
                                        hasMain = true
                                    }
                                }
                            }
                            "category" -> {
                                for (i in 0 until parser.attributeCount) {
                                    val attrVal = parser.getAttributeValue(i)
                                    if (parser.getAttributeName(i) == "name" &&
                                        (attrVal == "android.intent.category.LAUNCHER" ||
                                         attrVal == "android.intent.category.INFO" ||
                                         attrVal == "android.intent.category.LEANBACK_LAUNCHER")) {
                                        hasLauncher = true
                                    }
                                }
                            }
                            "meta-data" -> {
                                var mdName: String? = null
                                var mdVal: String? = null
                                var mdRes = 0
                                for (i in 0 until parser.attributeCount) {
                                    val attrName = parser.getAttributeName(i)
                                    if (attrName == "name") {
                                        mdName = parser.getAttributeValue(i)
                                    } else if (attrName == "value") {
                                        mdVal = parser.getAttributeValue(i)
                                    } else if (attrName == "resource") {
                                        mdRes = parser.getAttributeResourceValue(i, 0)
                                    }
                                }
                                if (!mdName.isNullOrEmpty()) {
                                    var resolved = false
                                    if (mdRes != 0) {
                                        try {
                                            val intVal = res.getInteger(mdRes)
                                            metaData.putInt(mdName, intVal)
                                            resolved = true
                                        } catch (_: Throwable) {}
                                        if (!resolved) {
                                            try {
                                                val strVal = res.getString(mdRes)
                                                val intVal = strVal.toIntOrNull()
                                                if (intVal != null) metaData.putInt(mdName, intVal)
                                                else metaData.putString(mdName, strVal)
                                                resolved = true
                                            } catch (_: Throwable) {}
                                        }
                                        if (!resolved) {
                                            metaData.putInt(mdName, mdRes)
                                        }
                                    } else if (mdVal != null) {
                                        if (mdVal.startsWith("@")) {
                                            val resId = mdVal.substring(1).toIntOrNull()
                                            if (resId != null) {
                                                try {
                                                    val intVal = res.getInteger(resId)
                                                    metaData.putInt(mdName, intVal)
                                                    resolved = true
                                                } catch (_: Throwable) {}
                                                if (!resolved) {
                                                    try {
                                                        val strVal = res.getString(resId)
                                                        val intVal = strVal.toIntOrNull()
                                                        if (intVal != null) metaData.putInt(mdName, intVal)
                                                        else metaData.putString(mdName, strVal)
                                                        resolved = true
                                                    } catch (_: Throwable) {}
                                                }
                                                if (!resolved) {
                                                    metaData.putInt(mdName, resId)
                                                }
                                            } else {
                                                metaData.putString(mdName, mdVal)
                                            }
                                        } else if (mdVal == "true" || mdVal == "false") {
                                            metaData.putBoolean(mdName, mdVal.toBoolean())
                                        } else {
                                            val intVal = mdVal.toIntOrNull()
                                            if (intVal != null) {
                                                metaData.putInt(mdName, intVal)
                                            } else {
                                                val floatVal = mdVal.toFloatOrNull()
                                                if (floatVal != null && mdVal.contains(".")) {
                                                    metaData.putFloat(mdName, floatVal)
                                                } else {
                                                    metaData.putString(mdName, mdVal)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    } else if (eventType == org.xmlpull.v1.XmlPullParser.END_TAG) {
                        if (parser.name == "activity") {
                            if (curActivityName != null) {
                                val full = if (curActivityName.startsWith(".")) {
                                    pkg + curActivityName
                                } else if (!curActivityName.contains(".")) {
                                    "$pkg.$curActivityName"
                                } else {
                                    curActivityName
                                }
                                activities[full] = ActivityMeta(full, curOrientation, curTheme)
                                if (hasMain && hasLauncher && launcherActivity == null) {
                                    launcherActivity = full
                                }
                            }
                            curActivityName = null
                        }
                    }
                    eventType = parser.next()
                }
                parser.close()
            }
        } catch (e: Throwable) {
            Log.w(TAG, "Failed to parse AndroidManifest.xml from ${apkFile.name}", e)
        }

        val fullAppClass = if (appClass != null) {
            if (appClass.startsWith(".")) pkg + appClass
            else if (!appClass.contains(".")) "$pkg.$appClass"
            else appClass
        } else null

        return ApkManifestInfo(
            packageName = pkg,
            versionName = verName,
            versionCode = verCode,
            applicationClass = fullAppClass,
            applicationTheme = appTheme,
            targetSdkVersion = targetSdk,
            launcherActivity = launcherActivity,
            activities = activities,
            providers = providers,
            services = services,
            receivers = receivers,
            metaData = metaData
        )
    }

    fun findLauncherActivity(apkFile: File, packageName: String): String? {
        return getManifestInfo(apkFile).launcherActivity
    }

    fun findApplicationClass(apkFile: File, packageName: String): String? {
        return getManifestInfo(apkFile).applicationClass
    }
}
