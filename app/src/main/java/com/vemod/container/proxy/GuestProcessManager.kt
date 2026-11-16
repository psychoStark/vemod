package com.vemod.container.proxy

import android.app.ActivityManager
import android.content.Context
import android.util.Log

object GuestProcessManager {
    private const val TAG = "GuestProcessManager"
    const val NUM_PROCESSES = 5
    private const val PREFS = "vemod_guest_processes"

    fun getTargetProxyClass(
        context: Context,
        packageName: String,
        orientation: Int = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
    ): Class<out GuestProxyActivity> {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val am = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
        val runningProcesses = am?.runningAppProcesses?.map { it.processName }?.toSet() ?: emptySet()

        // 1. Check if this package is already active in one of the slots
        for (i in 1..NUM_PROCESSES) {
            val assignedPkg = prefs.getString("proc_$i", null)
            val procName = "${context.packageName}:guest$i"
            if (assignedPkg == packageName && runningProcesses.contains(procName)) {
                Log.i(TAG, "Reusing running process :guest$i for $packageName (orientation=$orientation)")
                return getProxyClassForSlot(i, orientation)
            }
        }

        // 2. Find a free process slot whose process is not currently running
        for (i in 1..NUM_PROCESSES) {
            val procName = "${context.packageName}:guest$i"
            if (!runningProcesses.contains(procName)) {
                prefs.edit().putString("proc_$i", packageName).apply()
                Log.i(TAG, "Assigned idle process :guest$i to $packageName (orientation=$orientation)")
                return getProxyClassForSlot(i, orientation)
            }
        }

        // 3. If all 5 processes are active, assign LRU / least-used slot
        val lastUsedSlot = prefs.getInt("last_allocated_slot", 0)
        val nextSlot = (lastUsedSlot % NUM_PROCESSES) + 1
        prefs.edit()
            .putInt("last_allocated_slot", nextSlot)
            .putString("proc_$nextSlot", packageName)
            .apply()

        Log.i(TAG, "Allocated process :guest$nextSlot to $packageName (orientation=$orientation)")
        return getProxyClassForSlot(nextSlot, orientation)
    }

    fun getProxyClassForSlot(
        slot: Int,
        orientation: Int = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
    ): Class<out GuestProxyActivity> {
        val isPortrait = orientation == 1 || orientation == 7 || orientation == 9 || orientation == 12
        val isLandscape = orientation == 0 || orientation == 6 || orientation == 8 || orientation == 11

        return when (slot) {
            1 -> if (isPortrait) GuestProxyActivity1P::class.java else if (isLandscape) GuestProxyActivity1L::class.java else GuestProxyActivity1::class.java
            2 -> if (isPortrait) GuestProxyActivity2P::class.java else if (isLandscape) GuestProxyActivity2L::class.java else GuestProxyActivity2::class.java
            3 -> if (isPortrait) GuestProxyActivity3P::class.java else if (isLandscape) GuestProxyActivity3L::class.java else GuestProxyActivity3::class.java
            4 -> if (isPortrait) GuestProxyActivity4P::class.java else if (isLandscape) GuestProxyActivity4L::class.java else GuestProxyActivity4::class.java
            5 -> if (isPortrait) GuestProxyActivity5P::class.java else if (isLandscape) GuestProxyActivity5L::class.java else GuestProxyActivity5::class.java
            else -> if (isPortrait) GuestProxyActivity1P::class.java else if (isLandscape) GuestProxyActivity1L::class.java else GuestProxyActivity1::class.java
        }
    }
}
