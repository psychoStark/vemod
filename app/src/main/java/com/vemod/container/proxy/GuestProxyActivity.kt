package com.vemod.container.proxy

import android.app.Activity
import android.os.Bundle
import android.util.Log

/**
 * Manifest stub activity registered in android:process=":guest".
 * Real instantiation is intercepted by VemodInstrumentation.newActivity.
 */
class GuestProxyActivity : Activity() {

    companion object {
        const val EXTRA_CONTAINER_DIR = "extra_container_dir"
        const val EXTRA_BASE_APK = "extra_base_apk"
        const val EXTRA_PACKAGE_NAME = "extra_package_name"
        const val EXTRA_LAUNCH_ACTIVITY = "extra_launch_activity"
        const val EXTRA_SCREEN_ORIENTATION = "extra_screen_orientation"
        private const val TAG = "GuestProxyActivity"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(TAG, "GuestProxyActivity stub created")
    }
}
