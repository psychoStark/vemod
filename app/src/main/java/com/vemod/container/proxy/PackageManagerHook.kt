package com.vemod.container.proxy

import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.Bundle
import android.util.Log
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
        if (!meta.containsKey("com.google.android.gms.version")) {
            var gmsVer = 4132500
            try {
                val res = VemodInstrumentation.guestResources
                val id = res?.getIdentifier("google_play_services_version", "integer", targetPkg) ?: 0
                if (id != 0) {
                    gmsVer = res?.getInteger(id) ?: 4132500
                }
            } catch (_: Throwable) {}
            meta.putInt("com.google.android.gms.version", gmsVer)
        }
        if (!meta.containsKey("com.google.android.gms.games.APP_ID")) {
            meta.putString("com.google.android.gms.games.APP_ID", "123456789")
        }
    }

    override fun invoke(proxy: Any, method: Method, args: Array<out Any?>?): Any? {
        val methodName = method.name

        if (args != null && args.isNotEmpty()) {
            val targetPkg = args[0] as? String
            val guestPkg = VemodInstrumentation.activePackageName

            if (targetPkg != null && (targetPkg == guestPkg || targetPkg == hostPackage)) {
                if (methodName == "getApplicationInfo") {
                    try {
                        val realResult = method.invoke(real, *(args ?: emptyArray())) as? ApplicationInfo
                        val appInfo = if (realResult != null) ApplicationInfo(realResult) else ApplicationInfo()
                        appInfo.packageName = targetPkg
                        val meta = appInfo.metaData ?: Bundle()
                        populateMetaData(meta, targetPkg)
                        appInfo.metaData = meta
                        return appInfo
                    } catch (e: Throwable) {
                        val appInfo = ApplicationInfo()
                        appInfo.packageName = targetPkg
                        val meta = Bundle()
                        populateMetaData(meta, targetPkg)
                        appInfo.metaData = meta
                        return appInfo
                    }
                }

                if (methodName == "getPackageInfo") {
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
            throw e.targetException
        }
    }
}
