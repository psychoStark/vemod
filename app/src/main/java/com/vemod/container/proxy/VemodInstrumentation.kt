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
        var guestResources: android.content.res.Resources? = null
        @Volatile
        var guestAssets: android.content.res.AssetManager? = null
        @Volatile
        var guestClassLoader: ClassLoader? = null
        @Volatile
        var guestMetaData: Bundle? = null

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

        // If intent targets GuestProxyActivity or an AppSlot alias, let it proceed
        if (comp.className == GuestProxyActivity::class.java.name || comp.className.contains("AppSlot")) {
            return intent
        }

        // If intent targets a guest activity (not com.vemod.container or not GuestProxyActivity)
        if (comp.className != GuestProxyActivity::class.java.name) {
            val pkg = activePackageName
            val cDir = activeContainerDir
            val bApk = activeBaseApk

            if (pkg != null && cDir != null && bApk != null) {
                Log.i(TAG, "Routing intent for ${comp.className} -> GuestProxyActivity")
                val routed = Intent(intent)
                routed.setComponent(ComponentName("com.vemod.container", GuestProxyActivity::class.java.name))
                routed.putExtra(GuestProxyActivity.EXTRA_LAUNCH_ACTIVITY, comp.className)
                routed.putExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME, pkg)
                routed.putExtra(GuestProxyActivity.EXTRA_CONTAINER_DIR, cDir)
                routed.putExtra(GuestProxyActivity.EXTRA_BASE_APK, bApk)
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
        contextThread: IBinder,
        token: IBinder,
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
        contextThread: IBinder,
        token: IBinder,
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
        var launchActivity = intent.getStringExtra(GuestProxyActivity.EXTRA_LAUNCH_ACTIVITY)
        var containerPath = intent.getStringExtra(GuestProxyActivity.EXTRA_CONTAINER_DIR)
        var baseApkPath = intent.getStringExtra(GuestProxyActivity.EXTRA_BASE_APK)
        var pkgName = intent.getStringExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME)

        // If launched via AppSlot alias without explicit extras
        if (launchActivity == null) {
            val compName = intent.component?.className
            if (compName != null && compName.contains("AppSlot")) {
                try {
                    val appThreadClass = Class.forName("android.app.ActivityThread")
                    val currentApp = appThreadClass.getDeclaredMethod("currentApplication").invoke(null) as? android.app.Application
                    if (currentApp != null) {
                        val app = com.vemod.container.container.ContainerRegistry.getAppForSlot(currentApp, compName)
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
                            Log.i(TAG, "newActivity: resolved AppSlot $compName -> $pkgName ($launchActivity, orient=${app.screenOrientation})")
                        }
                    }
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to resolve AppSlot for $compName", e)
                }
            }
        }

        if (launchActivity != null && containerPath != null && baseApkPath != null) {
            Log.i(TAG, "newActivity: instantiating guest activity $launchActivity")
            activePackageName = pkgName
            activeContainerDir = containerPath
            activeBaseApk = baseApkPath

            val containerDir = File(containerPath)
            val baseApk = File(baseApkPath)
            val proxyDir = File(containerDir, "proxy")

            try {
                val pm = com.vemod.container.VemodApp.instance?.packageManager
                val archiveInfo = pm?.getPackageArchiveInfo(baseApk.absolutePath, android.content.pm.PackageManager.GET_META_DATA)
                guestMetaData = archiveInfo?.applicationInfo?.metaData
                Log.i(TAG, "Loaded guest metaData: $guestMetaData")
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to load guest metaData", e)
            }

            val guestLoader = GuestClassLoader(baseApk.absolutePath, proxyDir, cl)
            guestClassLoader = guestLoader
            Thread.currentThread().contextClassLoader = guestLoader

            VemodBridge.setActivePlugin(containerDir, guestLoader)

            if (guestAssets == null || guestResources == null) {
                try {
                    val am = android.content.res.AssetManager::class.java.getDeclaredConstructor().newInstance()
                    val addAssetPath = android.content.res.AssetManager::class.java.getDeclaredMethod("addAssetPath", String::class.java)
                    addAssetPath.invoke(am, baseApk.absolutePath)
                    guestAssets = am

                    val hostRes = android.content.res.Resources.getSystem()
                    guestResources = android.content.res.Resources(am, hostRes.displayMetrics, hostRes.configuration)
                    Log.i(TAG, "Initialized guest Resources and AssetManager from ${baseApk.absolutePath}")
                } catch (e: Throwable) {
                    Log.e(TAG, "Failed to initialize guest Resources", e)
                }
            }

            val guestClass = guestLoader.loadClass(launchActivity)
            return guestClass.getDeclaredConstructor().newInstance() as Activity
        }

        return base.newActivity(cl, className, intent)
    }

    override fun callActivityOnCreate(activity: Activity, icicle: Bundle?) {
        val intent = activity.intent
        val pkgName = intent?.getStringExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME) ?: activePackageName
        val containerPath = intent?.getStringExtra(GuestProxyActivity.EXTRA_CONTAINER_DIR) ?: activeContainerDir
        val baseApkPath = intent?.getStringExtra(GuestProxyActivity.EXTRA_BASE_APK) ?: activeBaseApk

        if (pkgName != null && containerPath != null && baseApkPath != null && activity !is GuestProxyActivity) {
            val containerDir = File(containerPath)
            val baseApk = File(baseApkPath)

            // Resolve and enforce the guest activity's declared screenOrientation
            try {
                var orientation = intent?.getIntExtra(
                    GuestProxyActivity.EXTRA_SCREEN_ORIENTATION,
                    android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                ) ?: android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED

                if (orientation == android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED) {
                    val pm = activity.packageManager
                    val archiveInfo = pm.getPackageArchiveInfo(
                        baseApk.absolutePath,
                        android.content.pm.PackageManager.GET_ACTIVITIES
                    )
                    val targetName = activity.javaClass.name
                    val actInfo = archiveInfo?.activities?.find { it.name == targetName }
                        ?: archiveInfo?.activities?.firstOrNull()
                    orientation = actInfo?.screenOrientation ?: android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                }

                if (orientation != android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED) {
                    activity.requestedOrientation = orientation
                    Log.i(TAG, "Applied declared screenOrientation $orientation to ${activity.javaClass.name}")
                }
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to apply screenOrientation to ${activity.javaClass.name}", e)
            }

            val guestLoader = activity.javaClass.classLoader ?: Thread.currentThread().contextClassLoader

            // Ensure guestResources has actual displayMetrics from activity context if available
            if (guestAssets != null) {
                val actRes = activity.baseContext?.resources ?: activity.resources
                guestResources = android.content.res.Resources(guestAssets, actRes.displayMetrics, actRes.configuration)
            }

            val vContext = GuestContext(
                base = activity.baseContext,
                guestPackageName = pkgName,
                containerDir = containerDir,
                baseApk = baseApk,
                guestLoader = guestLoader,
                preloadedAssets = guestAssets,
                preloadedResources = guestResources
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

            // Reset cached resources/theme on ContextThemeWrapper so it reads from GuestContext
            try {
                val resField = ContextThemeWrapper::class.java.getDeclaredField("mResources")
                resField.isAccessible = true
                resField.set(activity, null)
            } catch (_: Throwable) {}

            try {
                val themeField = ContextThemeWrapper::class.java.getDeclaredField("mTheme")
                themeField.isAccessible = true
                themeField.set(activity, null)
            } catch (_: Throwable) {}

            try {
                val inflaterField = ContextThemeWrapper::class.java.getDeclaredField("mInflater")
                inflaterField.isAccessible = true
                inflaterField.set(activity, null)
            } catch (_: Throwable) {}

            Log.i(TAG, "callActivityOnCreate: swapped context for ${activity.javaClass.name}")
        }

        base.callActivityOnCreate(activity, icicle)
        neutralizeGameHelper(activity)
    }

    override fun callActivityOnDestroy(activity: Activity) = base.callActivityOnDestroy(activity)

    override fun callActivityOnResume(activity: Activity) {
        base.callActivityOnResume(activity)
        neutralizeGameHelper(activity)
    }

    override fun callActivityOnPause(activity: Activity) = base.callActivityOnPause(activity)

    override fun callActivityOnStart(activity: Activity) {
        base.callActivityOnStart(activity)
        neutralizeGameHelper(activity)
    }

    override fun callActivityOnStop(activity: Activity) = base.callActivityOnStop(activity)

    private fun neutralizeGameHelper(activity: Activity) {
        if (activity is GuestProxyActivity) return
        try {
            if (activity.javaClass.name.contains("GameActivity")) {
                for (f in activity.javaClass.declaredFields) {
                    try {
                        f.isAccessible = true
                        if (f.name == "adInitialized" || f.name == "adVisible" || f.name == "adTestMode") {
                            if (f.type == java.lang.Boolean::class.java) {
                                f.set(null, java.lang.Boolean.FALSE)
                            }
                        }
                    } catch (_: Throwable) {}
                }
            }
        } catch (_: Throwable) {}
        try {
            var cls: Class<*>? = activity.javaClass
            while (cls != null && cls != Activity::class.java && cls != Any::class.java) {
                for (f in cls.declaredFields) {
                    try {
                        f.isAccessible = true
                        val obj = f.get(activity) ?: continue
                        val objName = obj.javaClass.name
                        if (objName.contains("GameHelper") || objName.startsWith("com.google.example.games.basegameutils.")) {
                            for (hf in obj.javaClass.declaredFields) {
                                try {
                                    hf.isAccessible = true
                                    // Field 'b' is the connection state enum (0: unconfigured, 1: disconnected, 2: connecting, 3: connected)
                                    // Setting it to 2 (connecting) forces beginUserInitiatedSignIn() (f()) to safely no-op
                                    // instead of attempting to show an error dialog from AndEngine's background render thread.
                                    if (hf.type == Int::class.javaPrimitiveType && hf.name == "b") {
                                        hf.setInt(obj, 2)
                                        Log.i(TAG, "neutralizeGameHelper: set ${objName}.b = 2 (STATE_CONNECTING)")
                                    }
                                    if (hf.type == Boolean::class.javaPrimitiveType && (hf.name == "l" || hf.name == "m")) {
                                        hf.setBoolean(obj, false)
                                    }
                                } catch (_: Throwable) {}
                            }
                        }
                    } catch (_: Throwable) {}
                }
                cls = cls.superclass
            }
        } catch (e: Throwable) {
            Log.w(TAG, "neutralizeGameHelper failed", e)
        }
    }
}
