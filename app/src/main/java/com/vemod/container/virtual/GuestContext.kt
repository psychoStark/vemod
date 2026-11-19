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

    override fun startActivity(intent: android.content.Intent) {
        try {
            val targetIntent = if (intent.action == android.content.Intent.ACTION_SEND) {
                android.content.Intent.createChooser(intent, null).apply {
                    addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
                }
            } else {
                android.content.Intent(intent).apply {
                    addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
                }
            }
            baseContext.startActivity(targetIntent)
        } catch (e: Throwable) {
            Log.w("GuestContext", "startActivity failed safely: ${e.message}")
        }
    }

    override fun startActivity(intent: android.content.Intent, options: android.os.Bundle?) {
        try {
            val targetIntent = if (intent.action == android.content.Intent.ACTION_SEND) {
                android.content.Intent.createChooser(intent, null).apply {
                    addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
                }
            } else {
                android.content.Intent(intent).apply {
                    addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
                }
            }
            baseContext.startActivity(targetIntent, options)
        } catch (e: Throwable) {
            Log.w("GuestContext", "startActivity with options failed safely: ${e.message}")
        }
    }

    override fun bindService(service: android.content.Intent, conn: android.content.ServiceConnection, flags: Int): Boolean {
        if (com.vemod.container.proxy.MockBillingService.isBillingOrLicensing(service)) {
            return com.vemod.container.proxy.MockBillingService.mockBind(service, conn)
        }
        if (isGoogleService(service)) {
            Log.w("GuestContext", "Preventing bindService to Google service: $service")
            return false
        }
        return try {
            super.bindService(service, conn, flags)
        } catch (e: Throwable) {
            Log.w("GuestContext", "Preventing crash in bindService: $service (${e.message})")
            false
        }
    }

    override fun startService(service: android.content.Intent): android.content.ComponentName? {
        if (isGoogleService(service)) {
            Log.w("GuestContext", "Preventing startService to Google service: $service")
            return null
        }
        return try {
            super.startService(service)
        } catch (e: Throwable) {
            Log.w("GuestContext", "Preventing crash in startService: $service (${e.message})")
            null
        }
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

    private var guestLocationManager: Any? = null

    private fun defaultReturnValue(returnType: Class<*>): Any? = when {
        returnType == java.lang.Boolean.TYPE -> false
        returnType == java.lang.Integer.TYPE -> 0
        returnType == java.lang.Long.TYPE -> 0L
        returnType == java.lang.Float.TYPE -> 0.0f
        returnType == java.lang.Double.TYPE -> 0.0
        returnType == java.lang.Byte.TYPE -> 0.toByte()
        returnType == java.lang.Short.TYPE -> 0.toShort()
        returnType == java.lang.Character.TYPE -> '\u0000'
        returnType == java.util.List::class.java -> emptyList<Any>()
        returnType == java.util.Set::class.java -> emptySet<Any>()
        returnType == java.util.Map::class.java -> emptyMap<Any, Any>()
        else -> null
    }

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
        if (name == Context.LOCATION_SERVICE) {
            val hostHasFine = baseContext.checkSelfPermission(android.Manifest.permission.ACCESS_FINE_LOCATION) == android.content.pm.PackageManager.PERMISSION_GRANTED
            val hostHasCoarse = baseContext.checkSelfPermission(android.Manifest.permission.ACCESS_COARSE_LOCATION) == android.content.pm.PackageManager.PERMISSION_GRANTED
            val realLocationManager = super.getSystemService(name) as? android.location.LocationManager
            if (realLocationManager != null && (hostHasFine || hostHasCoarse)) {
                return realLocationManager
            }
            if (guestLocationManager == null && realLocationManager != null) {
                try {
                    // LocationManager delegates to internal mService (ILocationManager).
                    // We proxy mService so getLastKnownLocation and other calls return null/safe stubs
                    // without throwing SecurityException when host lacks runtime location permissions.
                    var mServiceField: java.lang.reflect.Field? = null
                    var cls: Class<*>? = realLocationManager.javaClass
                    while (cls != null && cls != Any::class.java) {
                        try {
                            mServiceField = cls.getDeclaredField("mService")
                            break
                        } catch (_: NoSuchFieldException) {
                            cls = cls.superclass
                        }
                    }
                    if (mServiceField != null) {
                        mServiceField.isAccessible = true
                        val origService = mServiceField.get(realLocationManager)
                        if (origService != null) {
                            val allInterfaces = mutableSetOf<Class<*>>()
                            var sCls: Class<*>? = origService.javaClass
                            while (sCls != null) {
                                allInterfaces.addAll(sCls.interfaces)
                                sCls = sCls.superclass
                            }
                            val proxyService = java.lang.reflect.Proxy.newProxyInstance(
                                origService.javaClass.classLoader ?: javaClass.classLoader,
                                allInterfaces.toTypedArray()
                            ) { proxy, method, args ->
                                val mName = method.name
                                if (mName == "equals") {
                                    val other = args?.getOrNull(0)
                                    return@newProxyInstance proxy === other || origService == other
                                }
                                if (mName == "hashCode") {
                                    return@newProxyInstance origService.hashCode()
                                }
                                if (mName == "toString") {
                                    return@newProxyInstance origService.toString()
                                }
                                if (mName.startsWith("getLastLocation") || mName.startsWith("getCurrentLocation")) {
                                    defaultReturnValue(method.returnType)
                                } else if (mName.startsWith("requestLocationUpdates") || mName.startsWith("removeUpdates")) {
                                    defaultReturnValue(method.returnType)
                                } else if (mName == "isProviderEnabled") {
                                    false
                                } else if (mName.startsWith("getProviders") || mName.startsWith("getAllProviders")) {
                                    emptyList<String>()
                                } else {
                                    try {
                                        method.invoke(origService, *(args ?: emptyArray()))
                                    } catch (e: java.lang.reflect.InvocationTargetException) {
                                        if (e.targetException is SecurityException) {
                                            Log.w("GuestContext", "Suppressed SecurityException in ILocationManager.$mName: ${e.targetException.message}")
                                            defaultReturnValue(method.returnType)
                                        } else {
                                            throw e.targetException
                                        }
                                    } catch (e: SecurityException) {
                                        Log.w("GuestContext", "Suppressed SecurityException in ILocationManager.$mName: ${e.message}")
                                        defaultReturnValue(method.returnType)
                                    }
                                }
                            }
                            mServiceField.set(realLocationManager, proxyService)
                            Log.i("GuestContext", "Successfully wrapped LocationManager.mService with security proxy")
                        }
                    }
                    guestLocationManager = realLocationManager
                } catch (t: Throwable) {
                    Log.w("GuestContext", "Failed to proxy LocationManager mService", t)
                    guestLocationManager = realLocationManager
                }
            }
            return guestLocationManager ?: realLocationManager
        }
        if (name == Context.TELEPHONY_SERVICE || name == "telephony_registry") {
            val service = super.getSystemService(name)
            if (service != null) {
                try {
                    var cls: Class<*>? = service.javaClass
                    while (cls != null && cls != Any::class.java) {
                        for (f in cls.declaredFields) {
                            f.isAccessible = true
                            val orig = f.get(service) ?: continue
                            val typeName = orig.javaClass.name
                            if (typeName.contains("Telephony") || typeName.contains("ITelephony")) {
                                if (!java.lang.reflect.Proxy.isProxyClass(orig.javaClass)) {
                                    val allIfaces = mutableSetOf<Class<*>>()
                                    var c: Class<*>? = orig.javaClass
                                    while (c != null) {
                                        allIfaces.addAll(c.interfaces)
                                        c = c.superclass
                                    }
                                    val proxy = java.lang.reflect.Proxy.newProxyInstance(
                                        orig.javaClass.classLoader ?: javaClass.classLoader,
                                        allIfaces.toTypedArray()
                                    ) { p, method, args ->
                                        val mName = method.name
                                        if (mName == "equals") {
                                            val other = args?.getOrNull(0)
                                            return@newProxyInstance p === other || orig == other
                                        }
                                        if (mName == "hashCode") {
                                            return@newProxyInstance orig.hashCode()
                                        }
                                        if (mName == "toString") {
                                            return@newProxyInstance orig.toString()
                                        }
                                        if (mName.startsWith("listen")) {
                                            defaultReturnValue(method.returnType)
                                        } else {
                                            try {
                                                method.invoke(orig, *(args ?: emptyArray()))
                                            } catch (e: java.lang.reflect.InvocationTargetException) {
                                                if (e.targetException is SecurityException) {
                                                    Log.w("GuestContext", "Suppressed SecurityException in Telephony.$mName: ${e.targetException.message}")
                                                    defaultReturnValue(method.returnType)
                                                } else {
                                                    throw e.targetException
                                                }
                                            } catch (e: SecurityException) {
                                                Log.w("GuestContext", "Suppressed SecurityException in Telephony.$mName: ${e.message}")
                                                defaultReturnValue(method.returnType)
                                            }
                                        }
                                    }
                                    f.set(service, proxy)
                                    Log.i("GuestContext", "Successfully secured field ${f.name} in ${service.javaClass.simpleName}")
                                }
                            }
                        }
                        cls = cls.superclass
                    }
                } catch (t: Throwable) {
                    Log.w("GuestContext", "Failed to secure telephony service", t)
                }
            }
            return service
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
