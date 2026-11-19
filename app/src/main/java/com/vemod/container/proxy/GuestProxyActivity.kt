package com.vemod.container.proxy

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.util.Log

open class GuestProxyActivity : Activity() {

    companion object {
        const val EXTRA_CONTAINER_DIR = "extra_container_dir"
        const val EXTRA_BASE_APK = "extra_base_apk"
        const val EXTRA_PACKAGE_NAME = "extra_package_name"
        const val EXTRA_LAUNCH_ACTIVITY = "extra_launch_activity"
        const val EXTRA_SCREEN_ORIENTATION = "extra_screen_orientation"
        const val EXTRA_IS_DATA_EXPOSED = "extra_is_data_exposed"
        const val EXTRA_EXPOSED_DIR = "extra_exposed_dir"
        private const val TAG = "GuestProxyActivity"
    }

    override fun onUserLeaveHint() {
        val pkg = intent?.getStringExtra(EXTRA_PACKAGE_NAME)
        val isPipEnabled = if (!pkg.isNullOrEmpty()) {
            com.vemod.container.container.ContainerRegistry.isPipEnabledForApp(this, pkg)
        } else {
            val prefs = getSharedPreferences("vemod_settings", Context.MODE_PRIVATE)
            prefs.getBoolean("pip_enabled", false)
        }
        if (isPipEnabled && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            try {
                enterPictureInPictureMode(
                    android.app.PictureInPictureParams.Builder().build()
                )
            } catch (_: Throwable) {}
        }
        // Don't call super to prevent unintended system auto-PiP when disabled
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (this::class.java == GuestProxyActivity::class.java) {
            dispatchIntent(intent)
        }
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        if (this::class.java == GuestProxyActivity::class.java && intent != null) {
            dispatchIntent(intent)
        }
    }

    private fun dispatchIntent(srcIntent: Intent) {
        val pkg = srcIntent.getStringExtra(EXTRA_PACKAGE_NAME)
            ?: srcIntent.getStringExtra("extra_container_package")
            ?: srcIntent.data?.getQueryParameter("package")
            ?: srcIntent.data?.getQueryParameter("pkg")
            ?: ""

        if (pkg.isNotEmpty()) {
            com.vemod.container.container.ContainerRegistry.recordAppLaunch(this, pkg)
        }

        val app = if (pkg.isNotEmpty()) com.vemod.container.container.ContainerRegistry.getApp(this, pkg) else null
        if (app == null) {
            Log.w(TAG, "Container for '$pkg' does not exist or was uninstalled")
            if (pkg.isNotEmpty()) {
                android.widget.Toast.makeText(this, "App is not installed in Vemod", android.widget.Toast.LENGTH_LONG).show()
                com.vemod.container.container.StubApkGenerator.uninstallStub(this, pkg)
            }
            finish()
            return
        }

        val orientation = app.screenOrientation

        val targetClass = GuestProcessManager.getTargetProxyClass(this, pkg, orientation)
        Log.i(TAG, "Router: dispatching $pkg -> ${targetClass.simpleName} (orientation=$orientation)")

        val forwardIntent = Intent(this, targetClass).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            srcIntent.extras?.let { putExtras(it) }
            putExtra(EXTRA_PACKAGE_NAME, app.packageName)
            putExtra(EXTRA_CONTAINER_DIR, app.containerDir.absolutePath)
            putExtra(EXTRA_BASE_APK, app.baseApk.absolutePath)
            putExtra(EXTRA_LAUNCH_ACTIVITY, app.launchActivity)
            putExtra(EXTRA_SCREEN_ORIENTATION, app.screenOrientation)
            putExtra(EXTRA_IS_DATA_EXPOSED, app.isDataExposed)
            if (app.isDataExposed) {
                val exp = com.vemod.container.container.ContainerRegistry.getExposedDir(this@GuestProxyActivity, app)
                putExtra(EXTRA_EXPOSED_DIR, exp.absolutePath)
            }
        }
        startActivity(forwardIntent)
        finish()
        overridePendingTransition(0, 0)
    }
}

class GuestProxyActivity1 : GuestProxyActivity()
class GuestProxyActivity1P : GuestProxyActivity()
class GuestProxyActivity1L : GuestProxyActivity()

class GuestProxyActivity2 : GuestProxyActivity()
class GuestProxyActivity2P : GuestProxyActivity()
class GuestProxyActivity2L : GuestProxyActivity()

class GuestProxyActivity3 : GuestProxyActivity()
class GuestProxyActivity3P : GuestProxyActivity()
class GuestProxyActivity3L : GuestProxyActivity()

class GuestProxyActivity4 : GuestProxyActivity()
class GuestProxyActivity4P : GuestProxyActivity()
class GuestProxyActivity4L : GuestProxyActivity()

class GuestProxyActivity5 : GuestProxyActivity()
class GuestProxyActivity5P : GuestProxyActivity()
class GuestProxyActivity5L : GuestProxyActivity()
