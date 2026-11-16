package com.vemod.container.container

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.util.Log
import android.widget.Toast

class StubInstallReceiver : BroadcastReceiver() {
    companion object {
        private const val TAG = "StubInstallReceiver"
    }

    override fun onReceive(context: Context, intent: Intent) {
        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
        val message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)
        val pkg = intent.getStringExtra("package_name") ?: "App"

        when (status) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                val confirmIntent = intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
                if (confirmIntent != null) {
                    confirmIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    context.startActivity(confirmIntent)
                }
            }
            PackageInstaller.STATUS_SUCCESS -> {
                Log.i(TAG, "Successfully installed stub for $pkg")
                Toast.makeText(context, "$pkg added to App Drawer!", Toast.LENGTH_SHORT).show()
            }
            else -> {
                Log.w(TAG, "Install failed for $pkg: status=$status, msg=$message")
            }
        }
    }
}
