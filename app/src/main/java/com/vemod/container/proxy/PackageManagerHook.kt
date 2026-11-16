package com.vemod.container.proxy

import android.content.Context
import android.content.pm.ActivityInfo
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.content.pm.ProviderInfo
import android.content.pm.ServiceInfo
import android.os.Bundle
import android.util.Log
import com.vemod.container.container.ApkExtractor
import java.io.File
import java.lang.reflect.InvocationHandler
import java.lang.reflect.Method
import java.lang.reflect.Proxy

class PackageManagerHook(
    private val real: Any,
    private val hostPackage: String
) : InvocationHandler {

    companion object {
        private const val TAG = "PackageManagerHook"

        fun install(context: Context) {
            try {
                val activityThreadClass = Class.forName("android.app.ActivityThread")
                val getPmMethod = activityThreadClass.getDeclaredMethod("getPackageManager")
                getPmMethod.isAccessible = true
                val currentPm = getPmMethod.invoke(null) ?: return
                if (Proxy.isProxyClass(currentPm.javaClass)) return

                val ipmClass = Class.forName("android.content.pm.IPackageManager")
                val proxy = Proxy.newProxyInstance(
                    ipmClass.classLoader,
                    arrayOf(ipmClass),
                    PackageManagerHook(currentPm, context.packageName)
                )

                val sPmField = activityThreadClass.getDeclaredField("sPackageManager")
                sPmField.isAccessible = true
                sPmField.set(null, proxy)

                // Also update ContextImpl's ApplicationPackageManager cache
                try {
                    val pm = context.packageManager
                    val mPmField = pm.javaClass.getDeclaredField("mPM")
                    mPmField.isAccessible = true
                    mPmField.set(pm, proxy)
                } catch (_: Throwable) {}

                Log.i(TAG, "PackageManagerHook successfully installed")
            } catch (e: Throwable) {
                Log.e(TAG, "Failed to install PackageManagerHook", e)
            }
        }
    }

    private fun populateMetaData(meta: Bundle, targetPkg: String) {
        VemodInstrumentation.guestMetaData?.let {
            meta.putAll(it)
        }

        // Resolve any remaining string resource references like "@2131296260"
        for (key in meta.keySet().toList()) {
            val v = meta.get(key)
            if (v is String && v.startsWith("@")) {
                val id = v.substring(1).toIntOrNull()
                if (id != null) {
                    var resolved = false
                    try {
                        val res = VemodInstrumentation.guestResources
                        if (res != null) {
                            val intVal = res.getInteger(id)
                            meta.putInt(key, intVal)
                            resolved = true
                        }
                    } catch (_: Throwable) {}
                    if (!resolved) {
                        meta.putInt(key, id)
                    }
                }
            }
        }

        val gmsVerVal = meta.get("com.google.android.gms.version")
        if (gmsVerVal !is Int) {
            var gmsVer = 0
            if (gmsVerVal is String) {
                val str = gmsVerVal.trim()
                if (str.startsWith("@")) {
                    val id = str.substring(1).toIntOrNull()
                    if (id != null) {
                        try {
                            val res = VemodInstrumentation.guestResources
                            if (res != null) gmsVer = res.getInteger(id)
                        } catch (_: Throwable) {}
                    }
                } else {
                    gmsVer = str.toIntOrNull() ?: 0
                }
            }
            if (gmsVer == 0) {
                try {
                    val res = VemodInstrumentation.guestResources
                    val id = res?.getIdentifier("google_play_services_version", "integer", targetPkg) ?: 0
                    if (id != 0) {
                        gmsVer = res?.getInteger(id) ?: 0
                    }
                } catch (_: Throwable) {}
            }
            if (gmsVer == 0) gmsVer = 4132500
            meta.putInt("com.google.android.gms.version", gmsVer)
        }

        if (!meta.containsKey("com.google.android.gms.games.APP_ID")) {
            meta.putString("com.google.android.gms.games.APP_ID", "123456789")
        }
    }

    override fun invoke(proxy: Any, method: Method, args: Array<out Any?>?): Any? {
        val methodName = method.name

        if (methodName == "checkPermission" || methodName == "checkUidPermission") {
            return android.content.pm.PackageManager.PERMISSION_GRANTED
        }

        if (args != null && args.isNotEmpty()) {
            val guestPkg = VemodInstrumentation.activePackageName

            if (methodName == "getActivityInfo") {
                val comp = args[0] as? android.content.ComponentName
                val guestApk = VemodInstrumentation.activeBaseApk
                if (comp != null && guestApk != null) {
                    val isGuestPkg = comp.packageName.isEmpty() || comp.packageName == guestPkg || comp.packageName == hostPackage
                    val isGuestClass = comp.className.startsWith("com.vemod.container.proxy") ||
                                       (guestPkg != null && comp.className.startsWith(guestPkg))
                    if (isGuestPkg || isGuestClass) {
                        try {
                            val manifestInfo = ApkExtractor.getManifestInfo(File(guestApk))
                            val targetClass = if (comp.className.startsWith("com.vemod.container.proxy.GuestProxyActivity")) {
                                VemodInstrumentation.activeLaunchActivity ?: comp.className
                            } else {
                                comp.className
                            }
                            val actMeta = manifestInfo.activities[targetClass]
                                ?: manifestInfo.activities.values.firstOrNull()

                            val actInfo = ActivityInfo()
                            actInfo.packageName = if (comp.packageName.isNotEmpty()) comp.packageName else (guestPkg ?: hostPackage)
                            actInfo.name = comp.className
                            actInfo.configChanges = 0x40003FFF
                            actInfo.screenOrientation = actMeta?.screenOrientation ?: ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                            actInfo.theme = if (actMeta != null && actMeta.theme != 0) actMeta.theme else manifestInfo.applicationTheme
                            val meta = Bundle().apply { putAll(manifestInfo.metaData) }
                            populateMetaData(meta, actInfo.packageName)
                            actInfo.metaData = meta
                            actInfo.applicationInfo = ApplicationInfo().apply {
                                packageName = actInfo.packageName
                                metaData = meta
                            }
                            Log.i(TAG, "getActivityInfo: returning guest ActivityInfo for ${comp.className}")
                            return actInfo
                        } catch (e: Throwable) {
                            Log.w(TAG, "Failed in getActivityInfo hook", e)
                        }
                    }
                }
            }

            if (methodName == "resolveContentProvider") {
                val auth = args[0] as? String
                val guestApk = VemodInstrumentation.activeBaseApk
                if (auth != null && guestApk != null) {
                    try {
                        val manifestInfo = ApkExtractor.getManifestInfo(File(guestApk))
                        val prov = manifestInfo.providers.find { it.authority == auth || it.authority?.split(";")?.contains(auth) == true }
                        if (prov != null) {
                            val pi = ProviderInfo().apply {
                                packageName = guestPkg ?: hostPackage
                                name = prov.name
                                authority = prov.authority
                                processName = prov.processName ?: (guestPkg ?: hostPackage)
                                exported = prov.exported
                                applicationInfo = ApplicationInfo().apply { packageName = guestPkg ?: hostPackage }
                            }
                            Log.i(TAG, "resolveContentProvider: returning ProviderInfo for $auth -> ${prov.name}")
                            return pi
                        }
                    } catch (e: Throwable) {
                        Log.w(TAG, "Failed in resolveContentProvider hook", e)
                    }
                }
            }

            val targetPkg = args[0] as? String

            if (targetPkg != null && (targetPkg == guestPkg || targetPkg == hostPackage)) {
                if (methodName == "getInstallerPackageName") {
                    return "com.android.vending"
                }
                if (methodName == "getInstallSourceInfo") {
                    return null
                }
                if (methodName == "getApplicationInfo") {
                    val guestApk = VemodInstrumentation.activeBaseApk
                    val manifestInfo = if (guestApk != null) ApkExtractor.getManifestInfo(File(guestApk)) else null
                    val appInfo = ApplicationInfo().apply {
                        packageName = targetPkg
                        sourceDir = guestApk ?: ""
                        publicSourceDir = guestApk ?: ""
                        if (guestApk != null) {
                            val cDir = File(guestApk).parentFile
                            dataDir = File(cDir, "data").absolutePath
                            nativeLibraryDir = File(cDir, "lib32").absolutePath
                        }
                        if (manifestInfo != null) {
                            className = manifestInfo.applicationClass
                            theme = manifestInfo.applicationTheme
                            targetSdkVersion = manifestInfo.targetSdkVersion
                        }
                        val meta = Bundle()
                        if (manifestInfo != null) {
                            meta.putAll(manifestInfo.metaData)
                        }
                        populateMetaData(meta, targetPkg)
                        metaData = meta
                    }
                    return appInfo
                }

                if (methodName == "getPackageInfo") {
                    val guestApk = VemodInstrumentation.activeBaseApk
                    if (targetPkg == guestPkg && guestApk != null) {
                        try {
                            val manifestInfo = ApkExtractor.getManifestInfo(File(guestApk))
                            val pi = PackageInfo()
                            pi.packageName = targetPkg
                            pi.versionName = manifestInfo.versionName ?: "1.0"
                            val vCode = manifestInfo.versionCode ?: 1
                            pi.versionCode = vCode
                            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P) {
                                pi.longVersionCode = vCode.toLong()
                            }
                            pi.activities = manifestInfo.activities.values.map { act ->
                                ActivityInfo().apply {
                                    packageName = targetPkg
                                    name = act.name
                                    screenOrientation = act.screenOrientation
                                    theme = act.theme
                                    applicationInfo = ApplicationInfo().apply { packageName = targetPkg }
                                }
                            }.toTypedArray()
                            pi.providers = manifestInfo.providers.map { prov ->
                                ProviderInfo().apply {
                                    packageName = targetPkg
                                    name = prov.name
                                    processName = prov.processName ?: targetPkg
                                    authority = prov.authority
                                    exported = prov.exported
                                    applicationInfo = ApplicationInfo().apply { packageName = targetPkg }
                                }
                            }.toTypedArray()
                            pi.services = manifestInfo.services.map { svc ->
                                ServiceInfo().apply {
                                    packageName = targetPkg
                                    name = svc.name
                                    processName = svc.processName ?: targetPkg
                                    applicationInfo = ApplicationInfo().apply { packageName = targetPkg }
                                }
                            }.toTypedArray()
                            pi.receivers = manifestInfo.receivers.map { rec ->
                                ActivityInfo().apply {
                                    packageName = targetPkg
                                    name = rec.name
                                    processName = rec.processName ?: targetPkg
                                    applicationInfo = ApplicationInfo().apply { packageName = targetPkg }
                                }
                            }.toTypedArray()

                            val aInfo = ApplicationInfo().apply {
                                packageName = targetPkg
                                className = manifestInfo.applicationClass
                                theme = manifestInfo.applicationTheme
                                targetSdkVersion = manifestInfo.targetSdkVersion
                                sourceDir = guestApk
                                publicSourceDir = guestApk
                                val cDir = File(guestApk).parentFile
                                dataDir = File(cDir, "data").absolutePath
                                nativeLibraryDir = File(cDir, "lib32").absolutePath
                                val meta = Bundle().apply { putAll(manifestInfo.metaData) }
                                populateMetaData(meta, targetPkg)
                                metaData = meta
                            }
                            pi.applicationInfo = aInfo
                            Log.i(TAG, "getPackageInfo: returning synthetic PackageInfo for $targetPkg (activities=${pi.activities?.size ?: 0}, providers=${pi.providers?.size ?: 0})")
                            return pi
                        } catch (e: Throwable) {
                            Log.w(TAG, "Failed in getPackageInfo hook", e)
                        }
                    }

                    try {
                        val realResult = method.invoke(real, *(args ?: emptyArray())) as? PackageInfo
                        if (realResult != null) {
                            val aInfo = realResult.applicationInfo
                            if (aInfo != null) {
                                val meta = aInfo.metaData ?: Bundle()
                                populateMetaData(meta, targetPkg)
                                aInfo.metaData = meta
                            }
                            return realResult
                        }
                    } catch (_: Throwable) {}
                    return PackageInfo().apply {
                        packageName = targetPkg
                        versionCode = 1
                        versionName = "1.0"
                        applicationInfo = ApplicationInfo().apply {
                            this.packageName = targetPkg
                            val meta = Bundle()
                            populateMetaData(meta, targetPkg)
                            this.metaData = meta
                        }
                    }
                }
            }
        }

        return try {
            method.invoke(real, *(args ?: emptyArray()))
        } catch (e: java.lang.reflect.InvocationTargetException) {
            val target = e.targetException
            if (target is IllegalArgumentException && target.message?.contains("Unknown package") == true) {
                Log.w(TAG, "Swallowed Unknown package for $methodName: ${target.message}")
                if (method.returnType == java.lang.Boolean.TYPE) return false
                if (method.returnType == java.lang.Integer.TYPE) return 0
                return null
            }
            throw target
        }
    }
}
