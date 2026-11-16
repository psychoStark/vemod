package com.vemod.container.proxy

import android.app.Activity
import android.app.Instrumentation
import android.content.ComponentName
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import android.view.ContextThemeWrapper
import android.widget.Toast
import com.vemod.container.VemodApp
import com.vemod.container.bridge.VemodBridge
import com.vemod.container.virtual.GuestClassLoader
import com.vemod.container.virtual.GuestContext
import java.io.File
import java.lang.reflect.Method

class VemodInstrumentation(
    private val base: Instrumentation
) : Instrumentation() {

    companion object {
        private const val TAG = "VemodInstrumentation"

        @Volatile
        var activePackageName: String? = null
        @Volatile
        var activeContainerDir: String? = null
        @Volatile
        var activeBaseApk: String? = null
        @Volatile
        var activeLaunchActivity: String? = null
        @Volatile
        var activeProxyClassName: String? = null
        @Volatile
        var activeIsDataExposed: Boolean = false
        @Volatile
        var activeExposedDir: String? = null

        @Volatile
        var guestResources: android.content.res.Resources? = null
        @Volatile
        var guestAssets: android.content.res.AssetManager? = null
        @Volatile
        var guestClassLoader: ClassLoader? = null
        @Volatile
        var guestMetaData: Bundle? = null
        @Volatile
        var guestApplication: android.app.Application? = null
        @Volatile
        var activeResourcesLoader: android.content.res.loader.ResourcesLoader? = null

        fun createGuestResources(context: Context, baseApk: File): Pair<android.content.res.AssetManager, android.content.res.Resources> {
            try {
                // 1. Create isolated AssetManager with framework-res and baseApk (no host APK collision)
                val am = android.content.res.AssetManager::class.java.getDeclaredConstructor().newInstance()
                val addAssetPath = android.content.res.AssetManager::class.java.getDeclaredMethod("addAssetPath", String::class.java)
                val c1 = addAssetPath.invoke(am, "/system/framework/framework-res.apk") as? Int ?: 0
                val c2 = addAssetPath.invoke(am, baseApk.absolutePath) as? Int ?: 0
                Log.i(TAG, "createGuestResources: c1(framework)=$c1, c2(guest)=$c2 for ${baseApk.name}")

                try {
                    val getAssignedPackages = android.content.res.AssetManager::class.java.getDeclaredMethod("getAssignedPackageIdentifiers")
                    getAssignedPackages.isAccessible = true
                    val pkgMap = getAssignedPackages.invoke(am) as? android.util.SparseArray<*>
                    if (pkgMap != null) {
                        for (i in 0 until pkgMap.size()) {
                            val k = pkgMap.keyAt(i)
                            val v = pkgMap.valueAt(i)
                            Log.i(TAG, "  AssetManager AssignedPackage: 0x${Integer.toHexString(k)} -> $v")
                        }
                    }
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to getAssignedPackageIdentifiers: ${e.message}")
                }

                val hostRes = context.resources
                val res = android.content.res.Resources(am, hostRes.displayMetrics, hostRes.configuration)

                // 2. Attach ResourcesLoader for full Android 11+ layout XML and resource parsing
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                    try {
                        val pfd = android.os.ParcelFileDescriptor.open(baseApk, android.os.ParcelFileDescriptor.MODE_READ_ONLY)
                        val provider = android.content.res.loader.ResourcesProvider.loadFromApk(pfd)
                        val loader = android.content.res.loader.ResourcesLoader().apply {
                            addProvider(provider)
                        }
                        activeResourcesLoader = loader
                        res.addLoaders(loader)
                        Log.i(TAG, "Attached ResourcesLoader to isolated guest Resources for ${baseApk.name}")
                    } catch (e: Throwable) {
                        Log.w(TAG, "Failed to attach ResourcesLoader to guest Resources", e)
                    }
                }

                try {
                    val id1 = res.getIdentifier("windowActionBar", "attr", "com.estrongs.android.pop")
                    val id2 = res.getIdentifier("com.estrongs.android.pop:attr/windowActionBar", null, null)
                    Log.i(TAG, "createGuestResources IDs: id1=0x${Integer.toHexString(id1)}, id2=0x${Integer.toHexString(id2)}")
                } catch (e: Throwable) {
                    Log.w(TAG, "createGuestResources id check failed: ${e.message}")
                }

                return Pair(am, res)
            } catch (e: Throwable) {
                Log.e(TAG, "Failed to create guest Resources", e)
                val hostRes = context.resources
                return Pair(hostRes.assets, hostRes)
            }
        }

        fun install() {
            try {
                val activityThreadClass = Class.forName("android.app.ActivityThread")
                val currentActivityThreadMethod = activityThreadClass.getDeclaredMethod("currentActivityThread")
                currentActivityThreadMethod.isAccessible = true
                val mainThread = currentActivityThreadMethod.invoke(null) ?: return

                val instrumentationField = activityThreadClass.getDeclaredField("mInstrumentation")
                instrumentationField.isAccessible = true
                val currentInst = instrumentationField.get(mainThread) as? Instrumentation ?: return

                if (currentInst !is VemodInstrumentation) {
                    val wrapper = VemodInstrumentation(currentInst)
                    try {
                        val basicInitMethod = Instrumentation::class.java.getDeclaredMethod("basicInit", activityThreadClass)
                        basicInitMethod.isAccessible = true
                        basicInitMethod.invoke(wrapper, mainThread)
                    } catch (_: Throwable) {}

                    instrumentationField.set(mainThread, wrapper)
                    Log.i(TAG, "VemodInstrumentation successfully installed into ActivityThread")

                    // Catch unhandled background handler/looper crashes in guest threads
                    val prevHandler = Thread.getDefaultUncaughtExceptionHandler()
                    Thread.setDefaultUncaughtExceptionHandler { t, e ->
                        if (e.message?.contains("Can't create handler inside thread") == true) {
                            Log.w(TAG, "Suppressed background thread looper exception on ${t.name}", e)
                            return@setDefaultUncaughtExceptionHandler
                        }
                        prevHandler?.uncaughtException(t, e)
                    }
                }
            } catch (e: Throwable) {
                Log.e(TAG, "Failed to install VemodInstrumentation", e)
            }
        }
    }

    private fun route(intent: Intent?): Intent? {
        if (intent == null) return null
        val comp = intent.component ?: return intent

        // If intent targets GuestProxyActivity or any of its concurrent subclasses, let it proceed
        if (comp.className.startsWith("com.vemod.container.proxy.GuestProxyActivity")) {
            return intent
        }

        // If intent targets a guest activity (internal activity transition within guest app)
        val hostPkg = VemodApp.instance?.packageName ?: "com.psychostark.vemod"
        if (!comp.className.startsWith(hostPkg) && !comp.className.startsWith("com.vemod.container")) {
            val pkg = activePackageName
            val cDir = activeContainerDir
            val bApk = activeBaseApk

            if (pkg != null && cDir != null && bApk != null) {
                var targetGuestActivity = comp.className
                if (targetGuestActivity == "com.unity3d.player.UnityPlayerProxyActivity" ||
                    targetGuestActivity == "com.unity3d.player.UnityPlayerNativeActivity") {
                    Log.i(TAG, "Routing: auto-aliasing $targetGuestActivity -> com.unity3d.player.UnityPlayerActivity")
                    targetGuestActivity = "com.unity3d.player.UnityPlayerActivity"
                }
                val targetProxy = activeProxyClassName ?: GuestProxyActivity1::class.java.name
                Log.i(TAG, "Routing intent for ${comp.className} -> $targetProxy (guestActivity=$targetGuestActivity)")
                val routed = Intent(intent)
                routed.setComponent(ComponentName(hostPkg, targetProxy))
                routed.putExtra(GuestProxyActivity.EXTRA_LAUNCH_ACTIVITY, targetGuestActivity)
                routed.putExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME, pkg)
                routed.putExtra(GuestProxyActivity.EXTRA_CONTAINER_DIR, cDir)
                routed.putExtra(GuestProxyActivity.EXTRA_BASE_APK, bApk)
                routed.putExtra(GuestProxyActivity.EXTRA_IS_DATA_EXPOSED, activeIsDataExposed)
                if (activeExposedDir != null) {
                    routed.putExtra(GuestProxyActivity.EXTRA_EXPOSED_DIR, activeExposedDir)
                }
                val orient = intent.getIntExtra(GuestProxyActivity.EXTRA_SCREEN_ORIENTATION, android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED)
                if (orient != android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED) {
                    routed.putExtra(GuestProxyActivity.EXTRA_SCREEN_ORIENTATION, orient)
                }
                return routed
            }
        }
        return intent
    }

    // execStartActivity overload 1
    fun execStartActivity(
        who: Context,
        contextThread: IBinder?,
        token: IBinder?,
        target: Activity?,
        intent: Intent,
        requestCode: Int,
        options: Bundle?
    ): ActivityResult? {
        val routed = route(intent) ?: intent
        return try {
            val m = Instrumentation::class.java.getDeclaredMethod(
                "execStartActivity",
                Context::class.java,
                IBinder::class.java,
                IBinder::class.java,
                Activity::class.java,
                Intent::class.java,
                Int::class.javaPrimitiveType,
                Bundle::class.java
            )
            m.isAccessible = true
            m.invoke(base, who, contextThread, token, target, routed, requestCode, options) as? ActivityResult
        } catch (e: Throwable) {
            Log.e(TAG, "execStartActivity (Activity) failed", e)
            null
        }
    }

    // execStartActivity overload 2
    fun execStartActivity(
        who: Context,
        contextThread: IBinder?,
        token: IBinder?,
        target: String?,
        intent: Intent,
        requestCode: Int,
        options: Bundle?
    ): ActivityResult? {
        val routed = route(intent) ?: intent
        return try {
            val m = Instrumentation::class.java.getDeclaredMethod(
                "execStartActivity",
                Context::class.java,
                IBinder::class.java,
                IBinder::class.java,
                String::class.java,
                Intent::class.java,
                Int::class.javaPrimitiveType,
                Bundle::class.java
            )
            m.isAccessible = true
            m.invoke(base, who, contextThread, token, target, routed, requestCode, options) as? ActivityResult
        } catch (e: Throwable) {
            Log.e(TAG, "execStartActivity (String) failed", e)
            null
        }
    }

    override fun newActivity(cl: ClassLoader, className: String, intent: Intent): Activity {
        // If this is the base router activity in the main process, let it instantiate so it dispatches to GuestProxyActivityX
        if (className == GuestProxyActivity::class.java.name) {
            return base.newActivity(cl, className, intent)
        }

        Log.i(TAG, "newActivity: className=$className, extras=${intent.extras?.keySet()?.joinToString { "$it=${intent.extras?.get(it)}" }}")
        activeProxyClassName = className
        var launchActivity = intent.getStringExtra(GuestProxyActivity.EXTRA_LAUNCH_ACTIVITY)
        var containerPath = intent.getStringExtra(GuestProxyActivity.EXTRA_CONTAINER_DIR)
        var baseApkPath = intent.getStringExtra(GuestProxyActivity.EXTRA_BASE_APK)
        var pkgName = intent.getStringExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME)

        val isExposed = intent.getBooleanExtra(GuestProxyActivity.EXTRA_IS_DATA_EXPOSED, false)
        val expDir = intent.getStringExtra(GuestProxyActivity.EXTRA_EXPOSED_DIR)
        activeIsDataExposed = isExposed
        if (expDir != null) activeExposedDir = expDir

        // If launched via stub or package extra without explicit launchActivity
        if (launchActivity == null) {
            val requestedPkg = pkgName
                ?: intent.getStringExtra("extra_container_package")
                ?: intent.getStringExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME)
                ?: intent.data?.getQueryParameter("package")

            if (requestedPkg != null) {
                try {
                    val appThreadClass = Class.forName("android.app.ActivityThread")
                    val currentApp = appThreadClass.getDeclaredMethod("currentApplication").invoke(null) as? android.app.Application
                    if (currentApp != null) {
                        val app = com.vemod.container.container.ContainerRegistry.getApp(currentApp, requestedPkg)
                        if (app != null) {
                            launchActivity = app.launchActivity
                            containerPath = app.containerDir.absolutePath
                            baseApkPath = app.baseApk.absolutePath
                            pkgName = app.packageName
                            intent.putExtra(GuestProxyActivity.EXTRA_LAUNCH_ACTIVITY, launchActivity)
                            intent.putExtra(GuestProxyActivity.EXTRA_CONTAINER_DIR, containerPath)
                            intent.putExtra(GuestProxyActivity.EXTRA_BASE_APK, baseApkPath)
                            intent.putExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME, pkgName)
                            intent.putExtra(GuestProxyActivity.EXTRA_SCREEN_ORIENTATION, app.screenOrientation)
                            intent.putExtra(GuestProxyActivity.EXTRA_IS_DATA_EXPOSED, app.isDataExposed)
                            activeIsDataExposed = app.isDataExposed
                            if (app.isDataExposed) {
                                val exp = com.vemod.container.container.ContainerRegistry.getExposedDir(currentApp, app)
                                intent.putExtra(GuestProxyActivity.EXTRA_EXPOSED_DIR, exp.absolutePath)
                                activeExposedDir = exp.absolutePath
                            }
                            Log.i(TAG, "newActivity: resolved requested package $requestedPkg -> ($launchActivity, orient=${app.screenOrientation}, exposed=${app.isDataExposed})")
                        }
                    }
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to resolve package $requestedPkg", e)
                }
            }
        }

        if (launchActivity != null && containerPath != null && baseApkPath != null) {
            if (launchActivity.endsWith("UnityPlayerProxyActivity") ||
                launchActivity.endsWith("UnityPlayerNativeActivity")) {
                Log.i(TAG, "newActivity: auto-aliasing $launchActivity -> com.unity3d.player.UnityPlayerActivity")
                launchActivity = "com.unity3d.player.UnityPlayerActivity"
                intent.putExtra(GuestProxyActivity.EXTRA_LAUNCH_ACTIVITY, launchActivity)
            }
            Log.i(TAG, "newActivity: instantiating guest activity $launchActivity")
            activePackageName = pkgName
            activeContainerDir = containerPath
            activeBaseApk = baseApkPath
            activeLaunchActivity = launchActivity

            val containerDir = File(containerPath)
            val baseApk = File(baseApkPath)
            val proxyDir = File(containerDir, "proxy")

            try {
                val manifestInfo = com.vemod.container.container.ApkExtractor.getManifestInfo(baseApk)
                guestMetaData = manifestInfo.metaData
                Log.i(TAG, "Loaded guest metaData: $guestMetaData")
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to load guest metaData", e)
            }

            try {
                val hostLibDir = com.vemod.container.VemodApp.instance?.applicationInfo?.nativeLibraryDir
                if (hostLibDir != null) {
                    val hostProxy = File(hostLibDir, "libproxy.so")
                    if (hostProxy.exists() && proxyDir.exists()) {
                        proxyDir.listFiles { f -> f.extension == "so" }?.forEach { f ->
                            if (f.length() != hostProxy.length() || f.lastModified() < hostProxy.lastModified()) {
                                hostProxy.copyTo(f, overwrite = true)
                                Log.i(TAG, "Refreshed proxy stub ${f.name} (len ${f.length()} -> ${hostProxy.length()}) from ${hostProxy.absolutePath}")
                            }
                        }
                    }
                }
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to refresh proxy stubs", e)
            }

            val guestLoader = if (activePackageName == pkgName && guestClassLoader != null) {
                guestClassLoader!!
            } else {
                val loader = GuestClassLoader(baseApk.absolutePath, proxyDir, cl)
                guestClassLoader = loader
                loader
            }
            Thread.currentThread().contextClassLoader = guestLoader

            VemodBridge.setActivePlugin(containerDir, guestLoader)

            if (activePackageName != pkgName || guestAssets == null || guestResources == null) {
                guestAssets = null
                guestResources = null
                guestApplication = null
                activePackageName = pkgName

                val appCtx = VemodApp.instance ?: run {
                    try {
                        val atClass = Class.forName("android.app.ActivityThread")
                        atClass.getMethod("currentApplication").invoke(null) as? Context
                    } catch (_: Throwable) { null }
                }
                if (appCtx != null) {
                    val pair = createGuestResources(appCtx, baseApk)
                    guestAssets = pair.first
                    guestResources = pair.second
                }
            }
            fixCpuAndAbiSpoofing(guestLoader)

            if (launchActivity == "com.unity3d.player.UnityPlayerProxyActivity" ||
                launchActivity == "com.unity3d.player.UnityPlayerNativeActivity") {
                try {
                    guestLoader.loadClass("com.unity3d.player.UnityPlayerActivity")
                    Log.i(TAG, "newActivity: auto-aliasing $launchActivity -> com.unity3d.player.UnityPlayerActivity")
                    launchActivity = "com.unity3d.player.UnityPlayerActivity"
                    activeLaunchActivity = launchActivity
                } catch (_: Throwable) {}
            }

            val guestClass = guestLoader.loadClass(launchActivity)
            return guestClass.getDeclaredConstructor().newInstance() as Activity
        }

        return base.newActivity(cl, className, intent)
    }

    @android.annotation.SuppressLint("BlockedPrivateApi")
    private fun prepareGuestActivity(activity: Activity) {
        val intent = activity.intent
        val pkgName = intent?.getStringExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME) ?: activePackageName
        val containerPath = intent?.getStringExtra(GuestProxyActivity.EXTRA_CONTAINER_DIR) ?: activeContainerDir
        val baseApkPath = intent?.getStringExtra(GuestProxyActivity.EXTRA_BASE_APK) ?: activeBaseApk

        if (pkgName != null && containerPath != null && baseApkPath != null && activity !is GuestProxyActivity) {
            val containerDir = File(containerPath)
            val baseApk = File(baseApkPath)

            var declaredTheme = 0
            // Resolve and enforce the guest activity's declared screenOrientation and theme
            try {
                var orientation = intent?.getIntExtra(
                    GuestProxyActivity.EXTRA_SCREEN_ORIENTATION,
                    android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                ) ?: android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED

                val manifestInfo = com.vemod.container.container.ApkExtractor.getManifestInfo(baseApk)
                val targetName = activity.javaClass.name
                val actMeta = manifestInfo.activities[targetName]
                    ?: manifestInfo.activities.values.firstOrNull()

                if (orientation == android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED) {
                    orientation = actMeta?.screenOrientation ?: android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                }

                if (orientation != android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED) {
                    if (activity.requestedOrientation != orientation) {
                        activity.requestedOrientation = orientation
                        Log.i(TAG, "Applied declared screenOrientation $orientation to ${activity.javaClass.name}")
                    }
                    val isLandscape = orientation == 0 || orientation == 6 || orientation == 8 || orientation == 11
                    val isPortrait = orientation == 1 || orientation == 7 || orientation == 9 || orientation == 12
                    try {
                        val res = activity.resources
                        val dm = res.displayMetrics
                        val cfg = res.configuration
                        if (isLandscape && dm.widthPixels < dm.heightPixels) {
                            val temp = dm.widthPixels
                            dm.widthPixels = dm.heightPixels
                            dm.heightPixels = temp
                            cfg.orientation = android.content.res.Configuration.ORIENTATION_LANDSCAPE
                            @Suppress("DEPRECATION")
                            res.updateConfiguration(cfg, dm)
                            Log.i(TAG, "Aligned display metrics to landscape: ${dm.widthPixels}x${dm.heightPixels}")
                        } else if (isPortrait && dm.widthPixels > dm.heightPixels) {
                            val temp = dm.widthPixels
                            dm.widthPixels = dm.heightPixels
                            dm.heightPixels = temp
                            cfg.orientation = android.content.res.Configuration.ORIENTATION_PORTRAIT
                            @Suppress("DEPRECATION")
                            res.updateConfiguration(cfg, dm)
                            Log.i(TAG, "Aligned display metrics to portrait: ${dm.widthPixels}x${dm.heightPixels}")
                        }
                    } catch (e: Throwable) {
                        Log.w(TAG, "Failed to adjust display metrics for orientation", e)
                    }
                }

                declaredTheme = if (actMeta != null && actMeta.theme != 0) actMeta.theme else manifestInfo.applicationTheme
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to resolve activity info for ${activity.javaClass.name}", e)
            }

            val guestLoader = if (activePackageName == pkgName && guestClassLoader != null) {
                guestClassLoader!!
            } else {
                activity.javaClass.classLoader ?: Thread.currentThread().contextClassLoader
            }

            if (activePackageName != pkgName || guestAssets == null || guestResources == null) {
                guestAssets = null
                guestResources = null
                guestApplication = null
                activePackageName = pkgName

                val pair = createGuestResources(activity.baseContext ?: activity, baseApk)
                guestAssets = pair.first
                guestResources = pair.second
            }

            val isExposed = activity.intent?.getBooleanExtra(GuestProxyActivity.EXTRA_IS_DATA_EXPOSED, false)
                ?: activeIsDataExposed
            val expDirPath = activity.intent?.getStringExtra(GuestProxyActivity.EXTRA_EXPOSED_DIR)
                ?: activeExposedDir
            val expDir = if (expDirPath != null) File(expDirPath) else null

            val vContext = GuestContext(
                base = activity.baseContext,
                guestPackageName = pkgName,
                containerDir = containerDir,
                baseApk = baseApk,
                guestLoader = guestLoader,
                preloadedAssets = guestAssets,
                preloadedResources = guestResources,
                isDataExposed = isExposed,
                exposedDir = expDir
            )

            // Swap mBase to GuestContext
            try {
                var c: Class<*>? = ContextWrapper::class.java
                while (c != null) {
                    try {
                        val f = c.getDeclaredField("mBase")
                        f.isAccessible = true
                        f.set(activity, vContext)
                        break
                    } catch (_: NoSuchFieldException) {
                        c = c.superclass
                    }
                }
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to swap mBase", e)
            }

            var guestApp = guestApplication
            if (guestApp == null) {
                val manifestInfo = com.vemod.container.container.ApkExtractor.getManifestInfo(baseApk)
                val appClassName = manifestInfo.applicationClass
                Log.i(TAG, "Resolved guest application class: $appClassName for $pkgName")

                try {
                    val appClass = if (!appClassName.isNullOrEmpty()) {
                        guestLoader.loadClass(appClassName)
                    } else {
                        android.app.Application::class.java
                    }
                    val newApp = appClass.getDeclaredConstructor().newInstance() as android.app.Application
                    try {
                        val attachMethod = android.app.Application::class.java.getDeclaredMethod("attach", Context::class.java)
                        attachMethod.isAccessible = true
                        attachMethod.invoke(newApp, vContext)
                    } catch (e: Throwable) {
                        Log.w(TAG, "Application.attach failed: ${e.message}")
                    }

                    // Populate static application singleton fields for legacy apps (e.g. FexApplication.f)
                    var appCls: Class<*>? = newApp.javaClass
                    while (appCls != null && appCls != android.app.Application::class.java && appCls != Any::class.java) {
                        for (field in appCls.declaredFields) {
                            if (java.lang.reflect.Modifier.isStatic(field.modifiers)) {
                                if (field.type.isAssignableFrom(appCls) || field.type == Context::class.java || field.type == android.app.Application::class.java) {
                                    try {
                                        field.isAccessible = true
                                        if (field.get(null) == null) {
                                            field.set(null, newApp)
                                            Log.i(TAG, "Populated static singleton ${appCls.name}.${field.name} = $newApp")
                                        }
                                    } catch (_: Throwable) {}
                                }
                            }
                        }
                        appCls = appCls.superclass
                    }

                    try {
                        newApp.onCreate()
                    } catch (e: Throwable) {
                        Log.w(TAG, "Application.onCreate threw exception: ${e.message}", e)
                    }
                    guestApp = newApp
                    guestApplication = newApp
                    Log.i(TAG, "Initialized guest Application: ${newApp.javaClass.name}")
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to instantiate guest Application $appClassName", e)
                }
            }

            if (guestApp != null) {
                try {
                    var appCls: Class<*>? = guestApp.javaClass
                    while (appCls != null && appCls != android.app.Application::class.java && appCls != Any::class.java) {
                        for (field in appCls.declaredFields) {
                            if (java.lang.reflect.Modifier.isStatic(field.modifiers)) {
                                if (field.type.isAssignableFrom(appCls) || field.type == Context::class.java || field.type == android.app.Application::class.java) {
                                    try {
                                        field.isAccessible = true
                                        if (field.get(null) == null) {
                                            field.set(null, guestApp)
                                            Log.i(TAG, "Populated static singleton ${appCls.name}.${field.name} = $guestApp")
                                        }
                                    } catch (_: Throwable) {}
                                }
                            }
                        }
                        appCls = appCls.superclass
                    }
                } catch (_: Throwable) {}

                vContext.guestApplication = guestApp
                try {
                    val appField = Activity::class.java.getDeclaredField("mApplication")
                    appField.isAccessible = true
                    appField.set(activity, guestApp)
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to set activity.mApplication", e)
                }
                try {
                    val activityThreadClass = Class.forName("android.app.ActivityThread")
                    val currentActivityThreadMethod = activityThreadClass.getDeclaredMethod("currentActivityThread")
                    currentActivityThreadMethod.isAccessible = true
                    val mainThread = currentActivityThreadMethod.invoke(null)
                    if (mainThread != null) {
                        val mInitialAppField = activityThreadClass.getDeclaredField("mInitialApplication")
                        mInitialAppField.isAccessible = true
                        mInitialAppField.set(mainThread, guestApp)
                    }
                } catch (_: Throwable) {}
            }

            val guestInflater = android.view.LayoutInflater.from(vContext).cloneInContext(activity)

            try {
                val window = activity.window
                if (window != null) {
                    try {
                        val contextField = android.view.Window::class.java.getDeclaredField("mContext")
                        contextField.isAccessible = true
                        contextField.set(window, activity)
                    } catch (_: Throwable) {}
                    try {
                        val inflaterField = android.view.Window::class.java.getDeclaredField("mLayoutInflater")
                        inflaterField.isAccessible = true
                        inflaterField.set(window, guestInflater)
                    } catch (_: Throwable) {}
                    Log.i(TAG, "Updated Window mContext and mLayoutInflater with guestInflater for ${activity.javaClass.name}")
                }
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to update Window context/inflater", e)
            }

            // Reset cached resources/theme/inflater across activity and ContextThemeWrapper hierarchy
            var cls: Class<*>? = activity.javaClass
            while (cls != null && cls != Any::class.java) {
                try {
                    val f = cls.getDeclaredField("mResources")
                    f.isAccessible = true
                    f.set(activity, null)
                } catch (_: Throwable) {}
                try {
                    val f = cls.getDeclaredField("mTheme")
                    f.isAccessible = true
                    f.set(activity, null)
                } catch (_: Throwable) {}
                try {
                    val f = cls.getDeclaredField("mThemeResource")
                    f.isAccessible = true
                    f.setInt(activity, 0)
                } catch (_: Throwable) {}
                try {
                    val f = cls.getDeclaredField("mInflater")
                    f.isAccessible = true
                    f.set(activity, guestInflater)
                } catch (_: Throwable) {}
                try {
                    val f = cls.getDeclaredField("mLayoutInflater")
                    f.isAccessible = true
                    f.set(activity, guestInflater)
                } catch (_: Throwable) {}
                cls = cls.superclass
            }

            // If the guest app has a custom Resources wrapper singleton (e.g. ES File Explorer esclasses.g),
            // reset its cached static instance so it wraps the isolated guest Resources rather than host Resources.
            try {
                val gClass = guestLoader.loadClass("com.estrongs.android.pop.esclasses.g")
                for (field in gClass.declaredFields) {
                    if (java.lang.reflect.Modifier.isStatic(field.modifiers) && field.type == gClass) {
                        field.isAccessible = true
                        field.set(null, null)
                        Log.i(TAG, "Reset ${gClass.name}.${field.name} static singleton to null")
                    }
                }
            } catch (_: Throwable) {}


            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R && activeResourcesLoader != null) {
                try {
                    activity.resources.addLoaders(activeResourcesLoader)
                    Log.i(TAG, "Attached activeResourcesLoader to activity.resources (${activity.resources.javaClass.name})")
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to attach activeResourcesLoader to activity.resources", e)
                }
            }

            if (declaredTheme != 0) {
                try {
                    activity.setTheme(declaredTheme)
                    Log.i(TAG, "Applied declared theme 0x${Integer.toHexString(declaredTheme)} to ${activity.javaClass.name}")
                    try {
                        val a = activity.obtainStyledAttributes(intArrayOf(0x7f010040))
                        val hasVal = a.hasValue(0)
                        val boolVal = a.getBoolean(0, false)
                        Log.i(TAG, "Test windowActionBar (0x7f010040): hasValue=$hasVal, value=$boolVal")
                        a.recycle()

                        val rName = activity.resources.getResourceName(0x7f010040)
                        val rPkg = activity.resources.getResourcePackageName(0x7f010040)
                        val rType = activity.resources.getResourceTypeName(0x7f010040)
                        val rEntry = activity.resources.getResourceEntryName(0x7f010040)
                        Log.i(TAG, "DIAG 0x7f010040: name=$rName, pkg=$rPkg, type=$rType, entry=$rEntry")

                        val tv = android.util.TypedValue()
                        val resAttr = activity.theme.resolveAttribute(0x7f010040, tv, true)
                        Log.i(TAG, "DIAG resolveAttribute(0x7f010040): resolved=$resAttr, type=${tv.type}, data=${tv.data}")

                        val byNameId = activity.resources.getIdentifier("windowActionBar", "attr", pkgName)
                        val resAttrApp = activity.theme.resolveAttribute(byNameId, tv, true)
                        Log.i(TAG, "DIAG resolveAttribute(byNameId=0x${Integer.toHexString(byNameId)}): resolved=$resAttrApp, type=${tv.type}, data=${tv.data}")

                        val tvT = android.util.TypedValue()
                        val tTest1 = activity.resources.newTheme().apply { applyStyle(0x7f0900a1, true) }
                        val rA1 = tTest1.resolveAttribute(0x7f010040, tvT, true)
                        val tTest2 = activity.resources.newTheme().apply { applyStyle(0x7f09009d, true) }
                        val rA2 = tTest2.resolveAttribute(0x7f010040, tvT, true)
                        val tTest3 = activity.resources.newTheme().apply { applyStyle(0x7f090055, true) }
                        val rA3 = tTest3.resolveAttribute(0x7f010040, tvT, true)
                        val tTest4 = activity.resources.newTheme().apply { applyStyle(0x7f090118, true) }
                        val rA4 = tTest4.resolveAttribute(0x7f010040, tvT, true)
                        Log.i(TAG, "DIAG TEST: 0x7f0900a1=$rA1, 0x7f09009d=$rA2, 0x7f090055=$rA3, 0x7f090118=$rA4")

                        try {
                            tTest2.dump(Log.INFO, TAG, "TTEST2_DUMP: ")
                        } catch (e: Throwable) {
                            Log.w(TAG, "tTest2.dump failed: ${e.message}")
                        }
                    } catch (e: Throwable) {
                        Log.w(TAG, "Test windowActionBar exception: ${e.message}")
                    }
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to apply declared theme", e)
                }
            }

            val tvLight = android.util.TypedValue()
            val isLight = if (activity.theme.resolveAttribute(android.R.attr.isLightTheme, tvLight, true)) {
                tvLight.data != 0
            } else {
                false
            }

            // Fill in missing platform attributes with system defaults using force=false so guest theme customizations are preserved.
            // Use Light fallback for light themes so light styling (text colors, list backgrounds) isn't crushed by dark mode.
            try {
                val fallbackStyle = if (isLight) android.R.style.Theme_DeviceDefault_Light else android.R.style.Theme_DeviceDefault
                activity.theme.applyStyle(fallbackStyle, false)
                Log.i(TAG, "Applied ${if (isLight) "Theme_DeviceDefault_Light" else "Theme_DeviceDefault"} fallback (force=false) to activity.theme")
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to apply fallback theme", e)
            }

            // Explicitly synchronize windowBackground from the theme to activity.window (overriding #121212)
            try {
                val aBg = activity.theme.obtainStyledAttributes(intArrayOf(android.R.attr.windowBackground))
                if (aBg.hasValue(0)) {
                    val bgDrawable = aBg.getDrawable(0)
                    if (bgDrawable != null) {
                        activity.window.setBackgroundDrawable(bgDrawable)
                        Log.i(TAG, "Applied windowBackground from theme to window")
                    }
                } else if (isLight) {
                    activity.window.setBackgroundDrawable(android.graphics.drawable.ColorDrawable(android.graphics.Color.WHITE))
                    Log.i(TAG, "Applied default Color.WHITE to window for light theme")
                }
                aBg.recycle()
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to apply windowBackground from theme", e)
            }

            if (android.os.Build.VERSION.SDK_INT >= 29) {
                try {
                    val m = activity.window.javaClass.getMethod("setForceDarkAllowed", Boolean::class.javaPrimitiveType)
                    m.invoke(activity.window, false)
                } catch (_: Throwable) {}
            }

            try {
                var shouldDisableTitle = false
                try {
                    val a = activity.theme.obtainStyledAttributes(intArrayOf(
                        android.R.attr.windowNoTitle,
                        android.R.attr.windowActionBar,
                        android.R.attr.windowTitleStyle
                    ))
                    val noTitle = a.getBoolean(0, false)
                    val hasActionBar = a.getBoolean(1, false)
                    val hasTitleStyle = a.hasValue(2)
                    a.recycle()

                    val appNoTitleId = activity.resources.getIdentifier("windowNoTitle", "attr", pkgName)
                    var appNoTitle = false
                    if (appNoTitleId != 0) {
                        try {
                            val a2 = activity.theme.obtainStyledAttributes(intArrayOf(appNoTitleId))
                            appNoTitle = a2.getBoolean(0, false)
                            a2.recycle()
                        } catch (_: Throwable) {}
                    }

                    if (noTitle || appNoTitle || !hasTitleStyle || !hasActionBar) {
                        shouldDisableTitle = true
                    }
                } catch (_: Throwable) {
                    shouldDisableTitle = true
                }

                if (shouldDisableTitle) {
                    activity.requestWindowFeature(android.view.Window.FEATURE_NO_TITLE)
                    activity.window?.requestFeature(android.view.Window.FEATURE_NO_TITLE)
                    Log.i(TAG, "Requested FEATURE_NO_TITLE for ${activity.javaClass.name}")
                }
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to configure window title feature", e)
            }

            try {
                val sampleRes = activity.resources
                Log.i(TAG, "prepareGuestActivity: activity.resources = $sampleRes, assets=${sampleRes.assets}")
                Log.i(TAG, "prepareGuestActivity: vContext.resources = ${vContext.resources}, assets=${vContext.resources.assets}")
                val sampleParser = sampleRes.getLayout(0x7f030029)
                Log.i(TAG, "Verified sample layout 0x7f030029: $sampleParser")
                sampleParser.close()
            } catch (e: Throwable) {
                Log.w(TAG, "Test layout resolution 0x7f030029: ${e.message}")
            }

            val guestTargetSdk = try {
                com.vemod.container.container.ApkExtractor.getManifestInfo(baseApk).targetSdkVersion
            } catch (_: Throwable) { 28 }

            val effectiveSdk = if (guestTargetSdk > 0) guestTargetSdk else 28
            try {
                val vmRuntimeClass = Class.forName("dalvik.system.VMRuntime")
                val getRuntime = vmRuntimeClass.getDeclaredMethod("getRuntime")
                val runtime = getRuntime.invoke(null)
                val setTargetSdkVersion = vmRuntimeClass.getDeclaredMethod("setTargetSdkVersion", Int::class.javaPrimitiveType)
                setTargetSdkVersion.invoke(runtime, effectiveSdk)
                Log.i(TAG, "Applied VMRuntime.setTargetSdkVersion($effectiveSdk) for ${activity.javaClass.name}")
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to apply VMRuntime.setTargetSdkVersion", e)
            }

            try {
                activity.applicationInfo.targetSdkVersion = effectiveSdk
                activity.application?.applicationInfo?.targetSdkVersion = effectiveSdk
            } catch (_: Throwable) {}

            // Disable Android 12+ compat breaking gates (e.g. PendingIntent immutable flag requirement)
            try {
                val compatClass = Class.forName("android.compat.Compatibility")
                val delegateInterface = Class.forName("android.compat.Compatibility\$BehaviorChangeDelegate")
                val proxy = java.lang.reflect.Proxy.newProxyInstance(
                    delegateInterface.classLoader,
                    arrayOf(delegateInterface)
                ) { _, method, _ ->
                    if (method.name == "isChangeEnabled") false else null
                }
                val setDelegate = compatClass.getDeclaredMethod("setBehaviorChangeDelegate", delegateInterface)
                setDelegate.isAccessible = true
                setDelegate.invoke(null, proxy)
                Log.i(TAG, "Installed Compatibility.BehaviorChangeDelegate -> return false for breaking changes")
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to set Compatibility.BehaviorChangeDelegate", e)
            }

            try {
                activity.applicationInfo.targetSdkVersion = 23
                val app = activity.application
                if (app != null) {
                    app.applicationInfo.targetSdkVersion = 23
                    val appAm = app.getSystemService(Context.ALARM_SERVICE) as? android.app.AlarmManager
                    if (appAm != null) {
                        try {
                            val field = android.app.AlarmManager::class.java.getDeclaredField("mTargetSdkVersion")
                            field.isAccessible = true
                            field.setInt(appAm, 23)
                            Log.i(TAG, "Set Application AlarmManager.mTargetSdkVersion = 23")
                        } catch (_: Throwable) {}
                    }
                }
                ActivityManagerHook.install(activity)
                NotificationManagerHook.install(activity)
                val am = activity.getSystemService(Context.ALARM_SERVICE) as? android.app.AlarmManager
                if (am != null) {
                    val field = android.app.AlarmManager::class.java.getDeclaredField("mTargetSdkVersion")
                    field.isAccessible = true
                    field.setInt(am, 23)
                    Log.i(TAG, "Set AlarmManager.mTargetSdkVersion = 23 to prevent cancel(null) crash")
                }
                if (pkgName.startsWith("com.rovio.")) {
                    TouchPacer.install(activity)
                }
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to install system service hooks in callActivityOnCreate", e)
            }

            // Configure Unity graphics quality to Fantastic (level 5) to ensure high-poly models,
            // 4-bone skeletal skinning, full texture resolution, and no aggressive low-poly LOD culling.
            try {
                val prefs = activity.getSharedPreferences(pkgName, android.content.Context.MODE_PRIVATE)
                if (!prefs.contains("UnityGraphicsQuality")) {
                    prefs.edit().putInt("UnityGraphicsQuality", 5).apply()
                    Log.i(TAG, "Configured UnityGraphicsQuality = 5 (Fantastic) for $pkgName")
                }
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to configure UnityGraphicsQuality", e)
            }

            Log.i(TAG, "callActivityOnCreate: swapped context for ${activity.javaClass.name}")

            // Register back navigation callback: forwards back key events directly to native game engine
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU) {
                try {
                    activity.onBackInvokedDispatcher.registerOnBackInvokedCallback(
                        android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT
                    ) {
                        Log.d(TAG, "Intercepted system back gesture, forwarding KEYCODE_BACK to ${activity.javaClass.simpleName}")
                        val down = android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN, android.view.KeyEvent.KEYCODE_BACK)
                        val up = android.view.KeyEvent(android.view.KeyEvent.ACTION_UP, android.view.KeyEvent.KEYCODE_BACK)

                        var handled = false
                        try {
                            handled = activity.dispatchKeyEvent(down)
                            handled = activity.dispatchKeyEvent(up) || handled
                        } catch (e: Throwable) {
                            Log.w(TAG, "dispatchKeyEvent failed", e)
                        }

                        if (!handled) {
                            try {
                                handled = activity.onKeyDown(android.view.KeyEvent.KEYCODE_BACK, down)
                                handled = activity.onKeyUp(android.view.KeyEvent.KEYCODE_BACK, up) || handled
                            } catch (_: Throwable) {}
                        }

                        if (!handled) {
                            Log.d(TAG, "Back not consumed by native/view, invoking onBackPressed()")
                            try {
                                activity.onBackPressed()
                            } catch (_: Throwable) {
                                activity.finish()
                            }
                        }
                    }
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to register onBackInvokedCallback", e)
                }
            }
            try {
                val aiField = Activity::class.java.getDeclaredField("mActivityInfo")
                aiField.isAccessible = true
                val ai = aiField.get(activity) as? android.content.pm.ActivityInfo
                if (ai != null && declaredTheme != 0) {
                    ai.theme = declaredTheme
                }
                ai?.icon = 0
            } catch (_: Throwable) {}

            try {
                activity.window.setIcon(android.R.color.transparent)
            } catch (_: Throwable) {}
        }

        if (activity !is GuestProxyActivity) {
            fixCpuAndAbiSpoofing(activity.classLoader)
        }
    }

    private fun postGuestActivityOnCreate(activity: Activity) {
        AdSdkNeutralizer.neutralize(activity)

        // Show app-specific dynamic splash screen (real logo) instead of system blueprint icon
        if (activity !is GuestProxyActivity) {
            val intent = activity.intent
            val pkgName = intent?.getStringExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME) ?: activePackageName
            val containerPath = intent?.getStringExtra(GuestProxyActivity.EXTRA_CONTAINER_DIR) ?: activeContainerDir

            val cDir = containerPath ?: activeContainerDir
            val pName = pkgName ?: activePackageName
            if (cDir != null && pName != null) {
                val iconFile = File(cDir, "icon.png")
                val bmp = if (iconFile.exists()) {
                    android.graphics.BitmapFactory.decodeFile(iconFile.absolutePath)
                } else null

                // Set TaskDescription so Recent Apps displays the guest app's title and icon
                try {
                    val appThreadClass = Class.forName("android.app.ActivityThread")
                    val currentApp = appThreadClass.getDeclaredMethod("currentApplication").invoke(null) as? android.app.Application
                    val containerApp = if (currentApp != null) com.vemod.container.container.ContainerRegistry.getApp(currentApp, pName) else null
                    val label = containerApp?.appName ?: pName

                    @Suppress("DEPRECATION")
                    val td = android.app.ActivityManager.TaskDescription(
                        label,
                        bmp,
                        android.graphics.Color.parseColor("#121212")
                    )
                    activity.setTaskDescription(td)
                    Log.i(TAG, "Applied TaskDescription: label=$label, hasIcon=${bmp != null}")
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to apply TaskDescription", e)
                }



                if (bmp != null && activity !is android.app.NativeActivity) {
                    showSplashOverlay(activity, bmp)
                }
            }
        }
    }

    override fun callActivityOnCreate(activity: Activity, icicle: Bundle?) {
        prepareGuestActivity(activity)
        try {
            base.callActivityOnCreate(activity, icicle)
        } catch (e: Throwable) {
            val isGms = e.stackTrace.any {
                it.className.contains("google.android.gms") ||
                it.className.contains("GoogleAnalytics")
            } || (e.cause?.stackTrace?.any {
                it.className.contains("google.android.gms") ||
                it.className.contains("GoogleAnalytics")
            } == true)
            if (isGms && (e is NullPointerException || e.cause is NullPointerException)) {
                Log.w(TAG, "Swallowed GMS NullPointerException during callActivityOnCreate: ${e.message}")
            } else {
                throw e
            }
        }
        postGuestActivityOnCreate(activity)
    }

    override fun callActivityOnCreate(activity: Activity, icicle: Bundle?, persistentState: android.os.PersistableBundle?) {
        prepareGuestActivity(activity)
        try {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.LOLLIPOP) {
                base.callActivityOnCreate(activity, icicle, persistentState)
            } else {
                base.callActivityOnCreate(activity, icicle)
            }
        } catch (e: Throwable) {
            val isGms = e.stackTrace.any {
                it.className.contains("google.android.gms") ||
                it.className.contains("GoogleAnalytics")
            } || (e.cause?.stackTrace?.any {
                it.className.contains("google.android.gms") ||
                it.className.contains("GoogleAnalytics")
            } == true)
            if (isGms && (e is NullPointerException || e.cause is NullPointerException)) {
                Log.w(TAG, "Swallowed GMS NullPointerException during callActivityOnCreate: ${e.message}")
            } else {
                throw e
            }
        }
        postGuestActivityOnCreate(activity)
    }

    private fun showSplashOverlay(activity: Activity, iconBitmap: android.graphics.Bitmap) {
        try {
            val decorView = activity.window.decorView as? android.view.ViewGroup ?: return

            val splash = android.widget.FrameLayout(activity).apply {
                setBackgroundColor(android.graphics.Color.parseColor("#121212"))
                layoutParams = android.view.ViewGroup.LayoutParams(
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT
                )
            }

            val dp96 = (96 * activity.resources.displayMetrics.density).toInt()
            val imageView = android.widget.ImageView(activity).apply {
                setImageBitmap(iconBitmap)
                scaleType = android.widget.ImageView.ScaleType.FIT_CENTER
                layoutParams = android.widget.FrameLayout.LayoutParams(dp96, dp96).apply {
                    gravity = android.view.Gravity.CENTER
                }
            }

            splash.addView(imageView)
            decorView.addView(splash)

            // Smoothly fade out the splash overlay after 400ms
            splash.postDelayed({
                splash.animate()
                    .alpha(0f)
                    .setDuration(250)
                    .withEndAction {
                        try {
                            decorView.removeView(splash)
                        } catch (_: Throwable) {}
                    }
                    .start()
            }, 400)
        } catch (e: Throwable) {
            Log.w(TAG, "Failed to show splash overlay", e)
        }
    }

    override fun callActivityOnDestroy(activity: Activity) = base.callActivityOnDestroy(activity)

    override fun callActivityOnResume(activity: Activity) {
        try {
            base.callActivityOnResume(activity)
        } catch (e: SecurityException) {
            Log.w(TAG, "Swallowed SecurityException during onResume: ${e.message}")
        }
        AdSdkNeutralizer.neutralize(activity)
    }

    override fun callActivityOnPause(activity: Activity) {
        try {
            base.callActivityOnPause(activity)
        } catch (e: SecurityException) {
            Log.w(TAG, "Swallowed SecurityException during onPause: ${e.message}")
        }
    }

    override fun callActivityOnStart(activity: Activity) {
        try {
            base.callActivityOnStart(activity)
        } catch (e: SecurityException) {
            Log.w(TAG, "Swallowed SecurityException during onStart: ${e.message}")
        }
        AdSdkNeutralizer.neutralize(activity)
    }

    override fun callActivityOnStop(activity: Activity) = base.callActivityOnStop(activity)

    private fun neutralizeGameHelper(activity: Activity) {
        AdSdkNeutralizer.neutralize(activity)
    }

    private fun fixCpuAndAbiSpoofing(classLoader: ClassLoader?) {
        try {
            // 1. Spoof android.os.Build ABI fields to ARMv7
            try {
                val abiField = android.os.Build::class.java.getDeclaredField("CPU_ABI")
                abiField.isAccessible = true
                abiField.set(null, "armeabi-v7a")

                val abi2Field = android.os.Build::class.java.getDeclaredField("CPU_ABI2")
                abi2Field.isAccessible = true
                abi2Field.set(null, "armeabi")

                val supField = android.os.Build::class.java.getDeclaredField("SUPPORTED_ABIS")
                supField.isAccessible = true
                supField.set(null, arrayOf("armeabi-v7a", "armeabi"))

                val sup32Field = android.os.Build::class.java.getDeclaredField("SUPPORTED_32_BIT_ABIS")
                sup32Field.isAccessible = true
                sup32Field.set(null, arrayOf("armeabi-v7a", "armeabi"))
                Log.i(TAG, "fixCpuAndAbiSpoofing: spoofed Build ABI fields to armeabi-v7a")
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to spoof Build ABI fields", e)
            }

            // 2. Patch Unity CPU detection (UnityPlayer.Q / com.unity3d.player.b)
            if (classLoader != null) {
                try {
                    val upClass = Class.forName("com.unity3d.player.UnityPlayer", false, classLoader)
                    for (f in upClass.declaredFields) {
                        try {
                            f.isAccessible = true
                            val obj = f.get(null) ?: continue
                            val objClass = obj.javaClass
                            for (of in objClass.declaredFields) {
                                try {
                                    of.isAccessible = true
                                    if (of.type == Int::class.javaPrimitiveType) {
                                        val v = of.getInt(obj)
                                        // Feature bits: ARM(2) | ARMv5(4) | ARMv6(8) | ARMv7(16) | VFPv3(32) | NEON(64) | VFP(128) = 254 (0xFE)
                                        if (of.name == "a" || (v and 2) != 0) {
                                            val newVal = v or 0xFE
                                            of.setInt(obj, newVal)
                                            Log.i(TAG, "fixCpuAndAbiSpoofing: patched ${upClass.name}.${f.name}.${of.name} = $newVal")
                                        }
                                    }
                                } catch (_: Throwable) {}
                            }
                        } catch (_: Throwable) {}
                    }
                } catch (_: Throwable) {}
            }
        } catch (e: Throwable) {
            Log.w(TAG, "fixCpuAndAbiSpoofing failed", e)
        }
    }
}
