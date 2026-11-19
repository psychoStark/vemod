package com.vemod.container.proxy

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings
import android.util.Log

object MicroGManager {
    private const val TAG = "MicroGManager"

    const val GMS_PACKAGE = "com.google.android.gms"
    const val REVANCED_GMS_PACKAGE = "app.revanced.android.gms"
    const val VANCED_GMS_PACKAGE = "com.mgoogle.android.gms"
    const val STANDALONE_GMS_PACKAGE = "org.microg.gms.core"
    const val VENDING_PACKAGE = "com.android.vending"

    const val MICROG_FDROID_URL = "https://f-droid.org/packages/com.google.android.gms/"
    const val MICROG_RELEASES_URL = "https://github.com/microg/GmsCore/releases"

    val GMS_CANDIDATES = listOf(
        REVANCED_GMS_PACKAGE,
        STANDALONE_GMS_PACKAGE,
        VANCED_GMS_PACKAGE,
        GMS_PACKAGE
    )

    @Volatile
    private var cachedGmsPackage: String? = null
    @Volatile
    private var lastCheckTime: Long = 0

    fun getActiveMicroGPackage(context: Context): String? {
        val now = System.currentTimeMillis()
        val cached = cachedGmsPackage
        if (cached != null && (now - lastCheckTime) < 5000) {
            return cached
        }

        val pm = context.packageManager
        for (pkg in GMS_CANDIDATES) {
            try {
                if (pkg != GMS_PACKAGE) {
                    pm.getPackageInfo(pkg, 0)
                    Log.i(TAG, "Detected active microG provider: $pkg")
                    cachedGmsPackage = pkg
                    lastCheckTime = now
                    return pkg
                } else {
                    // For standard com.google.android.gms, inspect whether it is microG
                    val info = pm.getPackageInfo(
                        GMS_PACKAGE,
                        PackageManager.GET_SERVICES or PackageManager.GET_ACTIVITIES
                    )
                    val isMicroG = info.versionName?.contains("microg", ignoreCase = true) == true ||
                                   info.services?.any { it.name.contains("org.microg.") } == true ||
                                   info.activities?.any { it.name.contains("org.microg.") } == true
                    if (isMicroG) {
                        Log.i(TAG, "Detected standard package microG: $GMS_PACKAGE")
                        cachedGmsPackage = GMS_PACKAGE
                        lastCheckTime = now
                        return GMS_PACKAGE
                    }
                }
            } catch (_: PackageManager.NameNotFoundException) {
                // Not installed
            } catch (e: Throwable) {
                Log.w(TAG, "Error querying package $pkg: ${e.message}")
            }
        }

        cachedGmsPackage = null
        lastCheckTime = now
        return null
    }

    fun isMicroGInstalled(context: Context): Boolean {
        return getActiveMicroGPackage(context) != null
    }

    fun getMicroGDisplayName(context: Context): String {
        val pkg = getActiveMicroGPackage(context) ?: return "Not Installed"
        return when (pkg) {
            REVANCED_GMS_PACKAGE -> "ReVanced GmsCore ($pkg)"
            VANCED_GMS_PACKAGE -> "Vanced microG ($pkg)"
            STANDALONE_GMS_PACKAGE -> "microG GmsCore ($pkg)"
            GMS_PACKAGE -> "microG GmsCore (Spoofed/System)"
            else -> "microG ($pkg)"
        }
    }

    fun isGmsTarget(service: Intent?): Boolean {
        if (service == null) return false
        val pkg = service.component?.packageName ?: service.`package` ?: ""
        val action = service.action ?: ""
        return pkg in GMS_CANDIDATES ||
               pkg == VENDING_PACKAGE ||
               action.startsWith(GMS_PACKAGE) ||
               action.startsWith(REVANCED_GMS_PACKAGE) ||
               action.startsWith(VANCED_GMS_PACKAGE) ||
               action.startsWith(STANDALONE_GMS_PACKAGE)
    }

    fun rewriteGmsIntent(intent: Intent, context: Context): Intent {
        val activePkg = getActiveMicroGPackage(context) ?: return intent
        val rewritten = Intent(intent)

        // 1. Rewrite target package
        val targetPkg = intent.component?.packageName ?: intent.`package`
        if (targetPkg == null || targetPkg == GMS_PACKAGE || targetPkg in GMS_CANDIDATES) {
            rewritten.`package` = activePkg
        }

        // 2. Clear explicit component if pointing to an emulated GMS class so activePkg resolves it
        if (intent.component != null && intent.component?.packageName != activePkg) {
            rewritten.component = null
            rewritten.`package` = activePkg
        }

        // 3. Remap action string if microG is a fork with custom action namespace
        val action = intent.action
        if (action != null && activePkg != GMS_PACKAGE) {
            if (action.startsWith(GMS_PACKAGE)) {
                val candidateAction = action.replace(GMS_PACKAGE, activePkg)
                val testIntent = Intent(candidateAction).setPackage(activePkg)
                val hasSvc = context.packageManager.resolveService(testIntent, 0) != null
                val hasAct = context.packageManager.resolveActivity(testIntent, 0) != null
                if (hasSvc || hasAct) {
                    rewritten.action = candidateAction
                } else {
                    // Check if active package responds to original action
                    val origTest = Intent(action).setPackage(activePkg)
                    if (context.packageManager.resolveService(origTest, 0) == null &&
                        context.packageManager.resolveActivity(origTest, 0) == null) {
                        rewritten.action = candidateAction
                    }
                }
            }
        }

        return rewritten
    }

    fun createSettingsIntent(context: Context): Intent? {
        val activePkg = getActiveMicroGPackage(context) ?: return null
        val pm = context.packageManager

        // Candidate settings activity names
        val settingsActivities = listOf(
            ComponentName(activePkg, "org.microg.gms.ui.MainSettingsActivity"),
            ComponentName(activePkg, "org.microg.gms.ui.SettingsActivity"),
            ComponentName(activePkg, "org.microg.gms.ui.SettingsActivityLauncher")
        )

        for (comp in settingsActivities) {
            val testIntent = Intent(Intent.ACTION_MAIN).setComponent(comp)
            if (pm.resolveActivity(testIntent, 0) != null) {
                return testIntent.apply {
                    addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                }
            }
        }

        // Fallback to Application Details settings
        return Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
            data = Uri.parse("package:$activePkg")
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
    }

    fun createInstallIntent(): Intent {
        return Intent(Intent.ACTION_VIEW, Uri.parse(MICROG_RELEASES_URL)).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
    }
}
