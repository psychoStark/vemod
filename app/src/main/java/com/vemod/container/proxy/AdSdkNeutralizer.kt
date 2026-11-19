package com.vemod.container.proxy

import android.app.Activity
import android.util.Log

/**
 * AdSdkNeutralizer handles defunct legacy advertising, analytics, and gaming SDKs
 * (AdColony, Chartboost, Vungle, Flurry, MoPub, Tapjoy, GameHelper) that are embedded
 * in legacy 32-bit Android APKs.
 *
 * Prevents:
 * 1. Fatal crashes caused by calls to dead HTTP/AIDL endpoints.
 * 2. Background thread crashes from uninitialized Loopers or missing play services.
 * 3. Blocking modal dialogs on GL/AndEngine render threads.
 */
object AdSdkNeutralizer {
    private const val TAG = "AdSdkNeutralizer"

    fun neutralize(activity: Activity) {
        if (activity is GuestProxyActivity) return

        neutralizeGameHelper(activity)
        neutralizePrime31(activity)
        neutralizeAdColony(activity.classLoader)
        neutralizeChartboost(activity.classLoader)
        neutralizeFlurry(activity.classLoader)
        neutralizeVungle(activity.classLoader)
        neutralizeTapjoy(activity.classLoader)
        neutralizeMoPub(activity.classLoader)
        neutralizeAdMarvel(activity.classLoader)
        neutralizeInMobi(activity.classLoader)
        neutralizeEulaAndAdPreferences(activity)
        neutralizeGenericAdFlags(activity)
        neutralizeAdMob(activity)
    }

    private fun neutralizeEulaAndAdPreferences(activity: Activity) {
        try {
            // Generic privacy/EULA/consent preferences for legacy 32-bit games
            activity.getSharedPreferences("consent", android.content.Context.MODE_PRIVATE)
                .edit()
                .putBoolean("eula_accepted", true)
                .putBoolean("privacy_accepted", true)
                .putBoolean("gdpr_consent", true)
                .putBoolean("terms_accepted", true)
                .apply()
        } catch (e: Throwable) {
            Log.w(TAG, "neutralizeEulaAndAdPreferences error", e)
        }
    }

    private fun neutralizeInMobi(classLoader: ClassLoader) {
        try {
            val armCls = try {
                classLoader.loadClass("com.inmobi.commons.internal.ActivityRecognitionManager")
            } catch (_: ClassNotFoundException) { null }

            if (armCls != null) {
                for (f in armCls.declaredFields) {
                    try {
                        f.isAccessible = true
                        if (java.lang.reflect.Modifier.isStatic(f.modifiers)) {
                            if (f.type == Int::class.javaPrimitiveType) {
                                f.setInt(null, -1)
                            }
                        }
                    } catch (_: Throwable) {}
                }
                Log.i(TAG, "neutralizeInMobi: ActivityRecognitionManager neutralized")
            }

            val arsCls = try {
                classLoader.loadClass("com.inmobi.commons.internal.ActivityRecognitionSampler")
            } catch (_: ClassNotFoundException) { null }

            if (arsCls != null) {
                for (m in arsCls.declaredMethods) {
                    if (m.name.contains("stop", ignoreCase = true) && m.parameterTypes.isEmpty()) {
                        try {
                            m.isAccessible = true
                            m.invoke(null)
                            Log.i(TAG, "neutralizeInMobi: stopped ActivityRecognitionSampler")
                        } catch (_: Throwable) {}
                    }
                }
            }
        } catch (e: Throwable) {
            Log.w(TAG, "neutralizeInMobi error", e)
        }
    }

    private fun neutralizeAdMarvel(classLoader: ClassLoader) {
        try {
            val cls = classLoader.loadClass("com.admarvel.android.ads.AdMarvelUtils")
            for (m in cls.declaredMethods) {
                if (m.name.contains("Location", ignoreCase = true) && m.parameterTypes.size == 1 && m.parameterTypes[0] == Boolean::class.javaPrimitiveType) {
                    m.isAccessible = true
                    m.invoke(null, false)
                    Log.i(TAG, "neutralizeAdMarvel: disabled location via AdMarvelUtils.${m.name}(false)")
                }
            }
        } catch (_: Throwable) {}
    }

    private fun neutralizePrime31(activity: Activity) {
        try {
            val prefs = activity.getSharedPreferences("P31Prefs", android.content.Context.MODE_PRIVATE)
            val lastTx = prefs.getString("P31LASTTRANSACTIONKEY", null)
            if (lastTx != null) {
                prefs.edit().remove("P31LASTTRANSACTIONKEY").apply()
                Log.i(TAG, "neutralizePrime31: cleared P31LASTTRANSACTIONKEY")
            }
        } catch (e: Throwable) {
            Log.w(TAG, "neutralizePrime31 error", e)
        }
    }

    private fun neutralizeGameHelper(activity: Activity) {
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
                                    // Field 'b' is the connection state enum:
                                    // 0: unconfigured, 1: disconnected, 2: connecting, 3: connected.
                                    // Setting it to 2 (connecting) forces beginUserInitiatedSignIn()
                                    // to safely no-op instead of attempting to show an error dialog
                                    // from AndEngine's background render thread.
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
            Log.w(TAG, "neutralizeGameHelper error", e)
        }
    }

    private fun neutralizeGenericAdFlags(activity: Activity) {
        try {
            var cls: Class<*>? = activity.javaClass
            while (cls != null && cls != Activity::class.java && cls != Any::class.java) {
                for (f in cls.declaredFields) {
                    try {
                        f.isAccessible = true
                        val name = f.name.lowercase()
                        if (name.contains("adinitialized") || name.contains("advisible") || name.contains("adtestmode") || name.contains("bannerloaded")) {
                            if (f.type == java.lang.Boolean::class.java || f.type == Boolean::class.javaPrimitiveType) {
                                if (java.lang.reflect.Modifier.isStatic(f.modifiers)) {
                                    f.set(null, false)
                                } else {
                                    f.set(activity, false)
                                }
                            }
                        }
                    } catch (_: Throwable) {}
                }
                cls = cls.superclass
            }
        } catch (_: Throwable) {}
    }

    private fun neutralizeAdColony(classLoader: ClassLoader) {
        try {
            val adColonyClass = try {
                classLoader.loadClass("com.jirbo.adcolony.AdColony")
            } catch (_: ClassNotFoundException) {
                try {
                    classLoader.loadClass("com.adcolony.sdk.AdColony")
                } catch (_: ClassNotFoundException) {
                    null
                }
            } ?: return

            // Neutralize static config/ready flags if present
            for (f in adColonyClass.declaredFields) {
                try {
                    f.isAccessible = true
                    if (f.type == Boolean::class.javaPrimitiveType && java.lang.reflect.Modifier.isStatic(f.modifiers)) {
                        if (f.name.contains("enabled", ignoreCase = true) || f.name.contains("configured", ignoreCase = true)) {
                            f.setBoolean(null, false)
                        }
                    }
                } catch (_: Throwable) {}
            }
            Log.i(TAG, "AdColony neutralized")
        } catch (_: Throwable) {}
    }

    private fun neutralizeChartboost(classLoader: ClassLoader) {
        try {
            val cbClass = try {
                classLoader.loadClass("com.chartboost.sdk.Chartboost")
            } catch (_: ClassNotFoundException) {
                null
            } ?: return

            for (f in cbClass.declaredFields) {
                try {
                    f.isAccessible = true
                    if (f.type == Boolean::class.javaPrimitiveType && java.lang.reflect.Modifier.isStatic(f.modifiers)) {
                        if (f.name.contains("impressions", ignoreCase = true) || f.name.contains("visible", ignoreCase = true)) {
                            f.setBoolean(null, false)
                        }
                    }
                } catch (_: Throwable) {}
            }
            Log.i(TAG, "Chartboost neutralized")
        } catch (_: Throwable) {}
    }

    private fun neutralizeFlurry(classLoader: ClassLoader) {
        try {
            val flurryClass = try {
                classLoader.loadClass("com.flurry.android.FlurryAgent")
            } catch (_: ClassNotFoundException) {
                null
            } ?: return

            // Disable Flurry uncaught exception handling so it doesn't hook Thread.defaultUncaughtExceptionHandler
            try {
                val setCapture = flurryClass.getDeclaredMethod("setCaptureUncaughtExceptions", Boolean::class.javaPrimitiveType)
                setCapture.isAccessible = true
                setCapture.invoke(null, false)
                Log.i(TAG, "Flurry setCaptureUncaughtExceptions(false) invoked")
            } catch (_: Throwable) {}
        } catch (_: Throwable) {}
    }

    private fun neutralizeVungle(classLoader: ClassLoader) {
        try {
            val vungleClass = try {
                classLoader.loadClass("com.vungle.publisher.VunglePub")
            } catch (_: ClassNotFoundException) {
                null
            } ?: return

            for (f in vungleClass.declaredFields) {
                try {
                    f.isAccessible = true
                    if (f.type == Boolean::class.javaPrimitiveType && java.lang.reflect.Modifier.isStatic(f.modifiers)) {
                        f.setBoolean(null, false)
                    }
                } catch (_: Throwable) {}
            }
            Log.i(TAG, "Vungle neutralized")
        } catch (_: Throwable) {}
    }

    private fun neutralizeTapjoy(classLoader: ClassLoader) {
        try {
            val tjClass = try {
                classLoader.loadClass("com.tapjoy.Tapjoy")
            } catch (_: ClassNotFoundException) {
                null
            } ?: return

            for (f in tjClass.declaredFields) {
                try {
                    f.isAccessible = true
                    if (f.type == Boolean::class.javaPrimitiveType && java.lang.reflect.Modifier.isStatic(f.modifiers)) {
                        if (f.name.contains("connected", ignoreCase = true)) {
                            f.setBoolean(null, false)
                        }
                    }
                } catch (_: Throwable) {}
            }
            Log.i(TAG, "Tapjoy neutralized")
        } catch (_: Throwable) {}
    }

    private fun neutralizeMoPub(classLoader: ClassLoader) {
        try {
            val manifestUtilsCls = try {
                classLoader.loadClass("com.mopub.common.util.ManifestUtils")
            } catch (_: Throwable) {
                null
            }
            if (manifestUtilsCls != null) {
                for (f in manifestUtilsCls.declaredFields) {
                    try {
                        f.isAccessible = true
                        if (java.lang.reflect.Modifier.isStatic(f.modifiers)) {
                            if (f.type == Boolean::class.javaPrimitiveType) {
                                f.setBoolean(null, true)
                            } else if (java.util.List::class.java.isAssignableFrom(f.type)) {
                                val list = f.get(null)
                                if (list is java.util.List<*>) {
                                    try {
                                        @Suppress("UNCHECKED_CAST")
                                        (list as? java.util.List<Any?>)?.clear()
                                    } catch (_: Throwable) {
                                        f.set(null, java.util.ArrayList<Any?>())
                                    }
                                }
                            }
                        }
                    } catch (_: Throwable) {}
                }
                Log.i(TAG, "MoPub ManifestUtils neutralized")
            }
        } catch (e: Throwable) {
            Log.w(TAG, "neutralizeMoPub error", e)
        }
    }

    private fun neutralizeAdMob(activity: Activity) {
        fun cleanViews(view: android.view.View?) {
            if (view == null) return
            val className = view.javaClass.name
            if (className.contains("AdView", ignoreCase = true) ||
                className.contains("ViewSwitcher", ignoreCase = true) ||
                className.startsWith("com.google.android.gms.ads")
            ) {
                view.visibility = android.view.View.GONE
                try {
                    (view.parent as? android.view.ViewGroup)?.removeView(view)
                } catch (_: Throwable) {}
            }
            if (view is android.widget.TextView) {
                val txt = view.text?.toString() ?: ""
                if (txt.contains("Missing internet permission", ignoreCase = true) ||
                    txt.contains("Missing AdActivity", ignoreCase = true) ||
                    txt.contains("android.permission.INTERNET", ignoreCase = true)
                ) {
                    view.visibility = android.view.View.GONE
                    try {
                        val parent = view.parent as? android.view.ViewGroup
                        parent?.visibility = android.view.View.GONE
                        (parent?.parent as? android.view.ViewGroup)?.removeView(parent)
                    } catch (_: Throwable) {}
                }
            }
            if (view is android.view.ViewGroup) {
                for (i in 0 until view.childCount) {
                    cleanViews(view.getChildAt(i))
                }
            }
        }

        try {
            val content = activity.findViewById<android.view.ViewGroup>(android.R.id.content)
            if (content != null) {
                cleanViews(content)
                content.setOnHierarchyChangeListener(object : android.view.ViewGroup.OnHierarchyChangeListener {
                    override fun onChildViewAdded(parent: android.view.View?, child: android.view.View?) {
                        cleanViews(child)
                    }
                    override fun onChildViewRemoved(parent: android.view.View?, child: android.view.View?) {}
                })
            }
            activity.window?.decorView?.post {
                try {
                    cleanViews(activity.window?.decorView)
                } catch (_: Throwable) {}
            }
            Log.i(TAG, "AdMob neutralized")
        } catch (e: Throwable) {
            Log.w(TAG, "neutralizeAdMob error", e)
        }
    }
}
