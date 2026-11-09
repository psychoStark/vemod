package com.vemod.container.model

import android.content.pm.ActivityInfo
import java.io.File

data class ContainerApp(
    val packageName: String,
    val appName: String,
    val launchActivity: String,
    val versionName: String,
    val versionCode: Long,
    val containerDir: File,
    val baseApk: File,
    val lib32Dir: File,
    val proxyDir: File,
    val iconFile: File?,
    val nativeLibraries: List<String>,
    var slotIndex: Int = -1,
    val installedAt: Long = System.currentTimeMillis(),
    val screenOrientation: Int = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
)
