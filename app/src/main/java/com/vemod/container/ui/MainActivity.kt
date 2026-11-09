package com.vemod.container.ui

import android.content.Intent
import android.graphics.BitmapFactory
import android.os.Bundle
import android.util.Log
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vemod.container.container.ApkExtractor
import com.vemod.container.container.ContainerRegistry
import com.vemod.container.model.ContainerApp
import com.vemod.container.proxy.GuestProxyActivity
import java.io.File
import kotlin.concurrent.thread

class MainActivity : ComponentActivity() {

    companion object {
        private const val TAG = "MainActivity"
    }

    private var globalRefreshTrigger by mutableStateOf(0)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        handleInstallIntent(intent)

        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    ContainerHome()
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleInstallIntent(intent)
    }

    private fun handleInstallIntent(intent: Intent?) {
        val apkPath = intent?.getStringExtra("install_apk") ?: return
        thread {
            try {
                val file = File(apkPath)
                if (file.exists()) {
                    val app = ApkExtractor.extractFromFile(this@MainActivity, file)
                    ContainerRegistry.assignSlot(this@MainActivity, app.packageName)
                    runOnUiThread {
                        globalRefreshTrigger++
                        Toast.makeText(this@MainActivity, "Installed ${app.appName}!", Toast.LENGTH_SHORT).show()
                    }
                }
            } catch (e: Throwable) {
                Log.e(TAG, "Failed to auto-install APK from $apkPath", e)
            }
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    fun ContainerHome() {
        var isExtracting by remember { mutableStateOf(false) }
        var extractingName by remember { mutableStateOf("") }
        var installedApps by remember { mutableStateOf<List<ContainerApp>>(emptyList()) }
        var refreshTrigger by remember { mutableStateOf(0) }

        // Refresh installed apps list
        LaunchedEffect(refreshTrigger, globalRefreshTrigger) {
            installedApps = ContainerRegistry.getInstalledApps(this@MainActivity)
        }

        val apkPicker = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.GetContent()
        ) { uri ->
            if (uri != null) {
                isExtracting = true
                extractingName = "Extracting APK..."
                thread {
                    try {
                        val app = ApkExtractor.extract(this@MainActivity, uri)
                        // Auto-assign App Drawer slot if available
                        ContainerRegistry.assignSlot(this@MainActivity, app.packageName)
                        runOnUiThread {
                            isExtracting = false
                            refreshTrigger++
                            Toast.makeText(this@MainActivity, "Installed ${app.appName}!", Toast.LENGTH_SHORT).show()
                        }
                    } catch (e: Throwable) {
                        Log.e(TAG, "Failed to extract APK", e)
                        runOnUiThread {
                            isExtracting = false
                            Toast.makeText(this@MainActivity, "Install failed: ${e.message}", Toast.LENGTH_LONG).show()
                        }
                    }
                }
            }
        }

        Scaffold(
            topBar = {
                TopAppBar(
                    title = {
                        Column {
                            Text(
                                text = "Vemod",
                                fontWeight = FontWeight.Bold,
                                fontSize = 20.sp,
                                color = MaterialTheme.colorScheme.primary
                            )
                            Text(
                                text = "32-Bit APK Container Manager",
                                fontSize = 12.sp,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    },
                    actions = {
                        Button(
                            onClick = { apkPicker.launch("*/*") },
                            enabled = !isExtracting,
                            colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.primary),
                            shape = RoundedCornerShape(20.dp),
                            modifier = Modifier.padding(end = 8.dp)
                        ) {
                            Text("+ Install APK", fontSize = 13.sp)
                        }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.surface
                    )
                )
            }
        ) { paddingValues ->
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(paddingValues)
            ) {
                if (installedApps.isEmpty() && !isExtracting) {
                    EmptyStateView(
                        onInstallClick = { apkPicker.launch("*/*") },
                        onInstallPreload = { apkFile ->
                            isExtracting = true
                            extractingName = "Installing ${apkFile.name}..."
                            thread {
                                try {
                                    val app = ApkExtractor.extractFromFile(this@MainActivity, apkFile)
                                    ContainerRegistry.assignSlot(this@MainActivity, app.packageName)
                                    runOnUiThread {
                                        isExtracting = false
                                        refreshTrigger++
                                        Toast.makeText(this@MainActivity, "Installed ${app.appName}!", Toast.LENGTH_SHORT).show()
                                    }
                                } catch (e: Throwable) {
                                    Log.e(TAG, "Install failed", e)
                                    runOnUiThread {
                                        isExtracting = false
                                        Toast.makeText(this@MainActivity, "Install failed: ${e.message}", Toast.LENGTH_LONG).show()
                                    }
                                }
                            }
                        }
                    )
                } else {
                    LazyColumn(
                        modifier = Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(16.dp),
                        verticalArrangement = Arrangement.spacedBy(16.dp)
                    ) {
                        item {
                            Text(
                                text = "Installed 32-Bit Applications (${installedApps.size})",
                                fontSize = 14.sp,
                                fontWeight = FontWeight.SemiBold,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.padding(start = 4.dp, bottom = 4.dp)
                            )
                        }

                        items(installedApps, key = { it.packageName }) { app ->
                            AppCard(
                                app = app,
                                onLaunch = { launchApp(app) },
                                onPinShortcut = {
                                    val ok = ContainerRegistry.pinShortcut(this@MainActivity, app)
                                    if (ok) {
                                        Toast.makeText(this@MainActivity, "Shortcut requested for ${app.appName}", Toast.LENGTH_SHORT).show()
                                    } else {
                                        Toast.makeText(this@MainActivity, "Pin shortcut not supported by launcher", Toast.LENGTH_SHORT).show()
                                    }
                                },
                                onToggleSlot = {
                                    thread {
                                        ContainerRegistry.toggleAppDrawerSlot(this@MainActivity, app.packageName)
                                        runOnUiThread { refreshTrigger++ }
                                    }
                                },
                                onUninstall = {
                                    thread {
                                        ContainerRegistry.uninstallApp(this@MainActivity, app.packageName)
                                        runOnUiThread {
                                            refreshTrigger++
                                            Toast.makeText(this@MainActivity, "Uninstalled ${app.appName}", Toast.LENGTH_SHORT).show()
                                        }
                                    }
                                }
                            )
                        }
                    }
                }

                if (isExtracting) {
                    Box(
                        modifier = Modifier
                            .fillMaxSize()
                            .background(Color.Black.copy(alpha = 0.7f)),
                        contentAlignment = Alignment.Center
                    ) {
                        Card(
                            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
                            shape = RoundedCornerShape(16.dp)
                        ) {
                            Column(
                                modifier = Modifier.padding(24.dp),
                                horizontalAlignment = Alignment.CenterHorizontally
                            ) {
                                CircularProgressIndicator(color = MaterialTheme.colorScheme.primary)
                                Spacer(modifier = Modifier.height(16.dp))
                                Text(
                                    text = extractingName,
                                    fontSize = 14.sp,
                                    fontWeight = FontWeight.Medium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                        }
                    }
                }
            }
        }
    }

    @Composable
    fun AppCard(
        app: ContainerApp,
        onLaunch: () -> Unit,
        onPinShortcut: () -> Unit,
        onToggleSlot: () -> Unit,
        onUninstall: () -> Unit
    ) {
        val iconBitmap = remember(app.iconFile?.absolutePath) {
            if (app.iconFile != null && app.iconFile.exists()) {
                try {
                    BitmapFactory.decodeFile(app.iconFile.absolutePath)?.asImageBitmap()
                } catch (_: Throwable) {
                    null
                }
            } else null
        }

        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(16.dp),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.6f))
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    // App Icon
                    if (iconBitmap != null) {
                        Image(
                            bitmap = iconBitmap,
                            contentDescription = app.appName,
                            modifier = Modifier
                                .size(56.dp)
                                .clip(RoundedCornerShape(12.dp))
                        )
                    } else {
                        Box(
                            modifier = Modifier
                                .size(56.dp)
                                .clip(RoundedCornerShape(12.dp))
                                .background(MaterialTheme.colorScheme.primaryContainer),
                            contentAlignment = Alignment.Center
                        ) {
                            Text(
                                text = app.appName.take(1).uppercase(),
                                fontSize = 24.sp,
                                fontWeight = FontWeight.Bold,
                                color = MaterialTheme.colorScheme.onPrimaryContainer
                            )
                        }
                    }

                    Spacer(modifier = Modifier.width(16.dp))

                    Column(modifier = Modifier.weight(1f)) {
                        Text(
                            text = app.appName,
                            fontSize = 17.sp,
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.onSurface,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                        Text(
                            text = app.packageName,
                            fontSize = 11.sp,
                            color = MaterialTheme.colorScheme.outline,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                        Spacer(modifier = Modifier.height(4.dp))
                        Row(
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            // Version badge
                            Surface(
                                shape = RoundedCornerShape(4.dp),
                                color = MaterialTheme.colorScheme.surface
                            ) {
                                Text(
                                    text = "v${app.versionName}",
                                    fontSize = 10.sp,
                                    modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }

                            // App Drawer Slot Badge
                            if (app.slotIndex > 0) {
                                Surface(
                                    shape = RoundedCornerShape(4.dp),
                                    color = Color(0xFF1B5E20)
                                ) {
                                    Text(
                                        text = "App Drawer: Slot ${app.slotIndex}",
                                        fontSize = 10.sp,
                                        fontWeight = FontWeight.SemiBold,
                                        modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                                        color = Color(0xFFA5D6A7)
                                    )
                                }
                            }
                        }
                    }
                }

                Spacer(modifier = Modifier.height(12.dp))

                // Native Libraries
                if (app.nativeLibraries.isNotEmpty()) {
                    Text(
                        text = "Native 32-bit: ${app.nativeLibraries.joinToString(", ")}",
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.8f)
                    )
                    Spacer(modifier = Modifier.height(12.dp))
                }

                // Action Buttons Row
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Button(
                        onClick = onLaunch,
                        shape = RoundedCornerShape(10.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.primary),
                        modifier = Modifier.weight(1f)
                    ) {
                        Text("Launch", fontSize = 13.sp)
                    }

                    OutlinedButton(
                        onClick = onPinShortcut,
                        shape = RoundedCornerShape(10.dp),
                        contentPadding = PaddingValues(horizontal = 12.dp)
                    ) {
                        Text("Pin to Home", fontSize = 12.sp)
                    }

                    OutlinedButton(
                        onClick = onToggleSlot,
                        shape = RoundedCornerShape(10.dp),
                        contentPadding = PaddingValues(horizontal = 12.dp)
                    ) {
                        Text(if (app.slotIndex > 0) "Hide Drawer" else "Show Drawer", fontSize = 12.sp)
                    }

                    IconButton(
                        onClick = onUninstall,
                        modifier = Modifier.size(40.dp)
                    ) {
                        Text("🗑", fontSize = 16.sp)
                    }
                }
            }
        }
    }

    @Composable
    fun EmptyStateView(
        onInstallClick: () -> Unit,
        onInstallPreload: (File) -> Unit
    ) {
        val detectedApks = remember {
            val list = mutableListOf<File>()
            val localFlappy = File(filesDir, "flappy_bird.apk")
            if (localFlappy.exists()) list.add(localFlappy)

            val tmpFlappy = File("/data/local/tmp/flappy_bird.apk")
            if (tmpFlappy.exists() && !list.any { it.name == tmpFlappy.name }) list.add(tmpFlappy)
            list
        }

        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Box(
                modifier = Modifier
                    .size(72.dp)
                    .clip(CircleShape)
                    .background(MaterialTheme.colorScheme.primaryContainer),
                contentAlignment = Alignment.Center
            ) {
                Text("📦", fontSize = 32.sp)
            }

            Spacer(modifier = Modifier.height(20.dp))
            Text(
                text = "No 32-Bit Apps Installed",
                fontSize = 20.sp,
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.onSurface
            )
            Spacer(modifier = Modifier.height(8.dp))
            Text(
                text = "Install any legacy 32-bit (armeabi-v7a) APK to run it inside the isolated Dynarmic container. You can launch installed apps directly from the system App Drawer or Home Screen.",
                fontSize = 13.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                lineHeight = 18.sp,
                modifier = Modifier.padding(horizontal = 16.dp)
            )

            Spacer(modifier = Modifier.height(24.dp))
            Button(
                onClick = onInstallClick,
                shape = RoundedCornerShape(12.dp),
                colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.primary),
                modifier = Modifier.fillMaxWidth().height(48.dp)
            ) {
                Text("Pick and Install APK", fontSize = 14.sp)
            }

            if (detectedApks.isNotEmpty()) {
                Spacer(modifier = Modifier.height(16.dp))
                detectedApks.forEach { apk ->
                    OutlinedButton(
                        onClick = { onInstallPreload(apk) },
                        shape = RoundedCornerShape(12.dp),
                        modifier = Modifier.fillMaxWidth().height(44.dp)
                    ) {
                        Text("Preload ${apk.name}", fontSize = 13.sp)
                    }
                }
            }
        }
    }

    private fun launchApp(app: ContainerApp) {
        try {
            val am = getSystemService(android.content.Context.ACTIVITY_SERVICE) as? android.app.ActivityManager
            am?.runningAppProcesses?.forEach { p ->
                if (p.processName == "$packageName:guest") {
                    android.os.Process.killProcess(p.pid)
                }
            }
        } catch (_: Throwable) {}

        val targetClass = if (app.slotIndex > 0) {
            "com.vemod.container.slot.AppSlot${app.slotIndex}"
        } else {
            GuestProxyActivity::class.java.name
        }

        val intent = Intent().apply {
            setClassName(this@MainActivity, targetClass)
            putExtra(GuestProxyActivity.EXTRA_CONTAINER_DIR, app.containerDir.absolutePath)
            putExtra(GuestProxyActivity.EXTRA_BASE_APK, app.baseApk.absolutePath)
            putExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME, app.packageName)
            putExtra(GuestProxyActivity.EXTRA_LAUNCH_ACTIVITY, app.launchActivity)
            putExtra(GuestProxyActivity.EXTRA_SCREEN_ORIENTATION, app.screenOrientation)
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        startActivity(intent)
    }
}
