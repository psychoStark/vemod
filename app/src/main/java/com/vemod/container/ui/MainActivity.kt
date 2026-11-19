package com.vemod.container.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.util.Log
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.*
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.vemod.container.bridge.VemodBridge
import com.vemod.container.container.ApkExtractor
import com.vemod.container.container.ContainerRegistry
import com.vemod.container.container.StubApkGenerator
import com.vemod.container.model.ContainerApp
import com.vemod.container.proxy.GuestProxyActivity
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.concurrent.thread
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {

    companion object {
        private const val TAG = "MainActivity"
    }

    private var globalRefreshTrigger by mutableStateOf(0)

    override fun onResume() {
        super.onResume()
        globalRefreshTrigger++
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        handleInstallIntent(intent)

        setContent {
            val prefs = remember { getSharedPreferences("vemod_settings", Context.MODE_PRIVATE) }
            var themeMode by remember { mutableStateOf(prefs.getString("theme_mode", "system") ?: "system") }
            val systemInDark = isSystemInDarkTheme()
            val useDarkTheme = when (themeMode) {
                "dark" -> true
                "light" -> false
                else -> systemInDark
            }

            val context = LocalContext.current
            val dynamicColorsAvailable = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
            val colorScheme = when {
                dynamicColorsAvailable && useDarkTheme -> dynamicDarkColorScheme(context)
                dynamicColorsAvailable && !useDarkTheme -> dynamicLightColorScheme(context)
                useDarkTheme -> darkColorScheme()
                else -> lightColorScheme()
            }

            MaterialTheme(colorScheme = colorScheme) {
                ContainerHome(
                    themeMode = themeMode,
                    onThemeChange = { newMode ->
                        themeMode = newMode
                        prefs.edit().putString("theme_mode", newMode).apply()
                    },
                    useDarkTheme = useDarkTheme
                )
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleInstallIntent(intent)
    }

    private fun handleInstallIntent(intent: Intent?) {
        val apkPath = intent?.getStringExtra("install_apk")
        val viewUri = if (intent?.action == Intent.ACTION_VIEW) intent.data else null

        if (apkPath == null && viewUri == null) return

        thread {
            try {
                val app = if (viewUri != null) {
                    ApkExtractor.extract(this@MainActivity, viewUri)
                } else {
                    val file = File(apkPath!!)
                    if (!file.exists()) return@thread
                    ApkExtractor.extractFromFile(this@MainActivity, file)
                }
                ContainerRegistry.installToAppDrawer(this@MainActivity, app.packageName)
                runOnUiThread {
                    globalRefreshTrigger++
                    Toast.makeText(this@MainActivity, "Installed ${app.appName}!", Toast.LENGTH_SHORT).show()
                }
            } catch (e: Throwable) {
                Log.e(TAG, "Failed to auto-install package: ${e.message}", e)
                runOnUiThread {
                    Toast.makeText(this@MainActivity, "Failed to install: ${e.message}", Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    fun ContainerHome(
        themeMode: String,
        onThemeChange: (String) -> Unit,
        useDarkTheme: Boolean
    ) {
        val context = LocalContext.current
        val coroutineScope = rememberCoroutineScope()
        val haptic = LocalHapticFeedback.current
        val gridState = rememberLazyGridState()

        var isExtracting by remember { mutableStateOf(false) }
        var extractingName by remember { mutableStateOf("") }
        var installedApps by remember { mutableStateOf<List<ContainerApp>>(emptyList()) }
        var refreshTrigger by remember { mutableStateOf(0) }
        var showSettingsDialog by remember { mutableStateOf(false) }
        var showSortMenu by remember { mutableStateOf(false) }
        var selectedAppForActions by remember { mutableStateOf<ContainerApp?>(null) }
        var selectedAppForInfo by remember { mutableStateOf<ContainerApp?>(null) }
        var appToUninstall by remember { mutableStateOf<ContainerApp?>(null) }
        var isSearchActive by remember { mutableStateOf(false) }
        var searchQuery by remember { mutableStateOf("") }

        val prefs = remember { getSharedPreferences("vemod_settings", Context.MODE_PRIVATE) }
        var currentLogLevel by remember { mutableStateOf(prefs.getInt("log_level", VemodBridge.LOG_LEVEL_BASIC)) }
        var pipEnabled by remember { mutableStateOf(prefs.getBoolean("pip_enabled", false)) }
        var sortMode by remember { mutableStateOf(prefs.getString("app_sort_mode", "alphabetical") ?: "alphabetical") }

        // Refresh installed apps list
        LaunchedEffect(refreshTrigger, globalRefreshTrigger) {
            installedApps = ContainerRegistry.getInstalledApps(this@MainActivity)
        }

        // Apply sorting
        val sortedApps = remember(installedApps, sortMode, refreshTrigger, globalRefreshTrigger) {
            when (sortMode) {
                "recently_played" -> installedApps.sortedWith(
                    compareByDescending<ContainerApp> {
                        val last = ContainerRegistry.getLastLaunchTime(context, it.packageName)
                        if (last > 0) last else it.installedAt
                    }.thenByDescending { it.installedAt }
                )
                "install_date" -> installedApps.sortedByDescending { it.installedAt }
                else -> installedApps.sortedBy { it.appName.lowercase() }
            }
        }

        // Filter based on search query
        val filteredApps = remember(sortedApps, searchQuery) {
            if (searchQuery.isBlank()) {
                sortedApps
            } else {
                sortedApps.filter {
                    it.appName.contains(searchQuery, ignoreCase = true) ||
                            it.packageName.contains(searchQuery, ignoreCase = true)
                }
            }
        }

        // Fast scrollbar visibility & dragging state
        var isDraggingThumb by remember { mutableStateOf(false) }
        var isScrollbarVisible by remember { mutableStateOf(false) }

        LaunchedEffect(gridState.isScrollInProgress, isDraggingThumb) {
            if (gridState.isScrollInProgress || isDraggingThumb) {
                isScrollbarVisible = true
            } else {
                delay(1200)
                isScrollbarVisible = false
            }
        }

        val firstVisibleIdx by remember { derivedStateOf { gridState.firstVisibleItemIndex } }
        val activeSectionLetter = remember(firstVisibleIdx, filteredApps, sortMode) {
            if (sortMode == "alphabetical") {
                filteredApps.getOrNull(firstVisibleIdx)?.appName?.firstOrNull()?.uppercaseChar()
            } else null
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
                        // Auto-install App Drawer stub
                        ContainerRegistry.installToAppDrawer(this@MainActivity, app.packageName)
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

        val manageStorageLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.StartActivityForResult()
        ) {
            refreshTrigger++
        }

        var appRemovingStub by remember { mutableStateOf<ContainerApp?>(null) }
        val stubUninstallLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.StartActivityForResult()
        ) {
            val app = appRemovingStub
            appRemovingStub = null
            refreshTrigger++
            globalRefreshTrigger++
            if (app != null) {
                if (!ContainerRegistry.isAppInAppDrawer(this@MainActivity, app.packageName)) {
                    Toast.makeText(this@MainActivity, "Removed ${app.appName} from apps", Toast.LENGTH_SHORT).show()
                }
            }
        }

        // Frosted glass background brush
        val backgroundBrush = if (useDarkTheme) {
            Brush.verticalGradient(
                listOf(
                    Color(0xFF090A0E),
                    Color(0xFF10131B),
                    Color(0xFF090A0E)
                )
            )
        } else {
            Brush.verticalGradient(
                listOf(
                    Color(0xFFF7F8FC),
                    Color(0xFFEDF1F7),
                    Color(0xFFE5EAF2)
                )
            )
        }

        Scaffold(
            topBar = {
                Surface(
                    color = if (useDarkTheme) Color(0xCC090A0E) else Color(0xCCF7F8FC),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    TopAppBar(
                        title = {
                            if (isSearchActive) {
                                OutlinedTextField(
                                    value = searchQuery,
                                    onValueChange = { searchQuery = it },
                                    placeholder = { Text("Search APKs...", fontSize = 14.sp) },
                                    singleLine = true,
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .padding(end = 8.dp),
                                    shape = RoundedCornerShape(24.dp),
                                    colors = OutlinedTextFieldDefaults.colors(
                                        focusedBorderColor = MaterialTheme.colorScheme.primary,
                                        unfocusedBorderColor = MaterialTheme.colorScheme.outline.copy(alpha = 0.3f),
                                        focusedContainerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.5f),
                                        unfocusedContainerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.3f)
                                    ),
                                    trailingIcon = {
                                        if (searchQuery.isNotEmpty()) {
                                            IconButton(onClick = { searchQuery = "" }) {
                                                Icon(Icons.Default.Close, contentDescription = "Clear")
                                            }
                                        }
                                    }
                                )
                            } else {
                                Row(
                                    verticalAlignment = Alignment.CenterVertically,
                                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                                ) {
                                    Text(
                                        text = "Vemod",
                                        fontWeight = FontWeight.Bold,
                                        fontSize = 22.sp,
                                        color = MaterialTheme.colorScheme.onSurface
                                    )
                                    if (installedApps.isNotEmpty()) {
                                        Surface(
                                            shape = CircleShape,
                                            color = MaterialTheme.colorScheme.primaryContainer
                                        ) {
                                            Text(
                                                text = "${installedApps.size}",
                                                fontSize = 11.sp,
                                                fontWeight = FontWeight.Bold,
                                                color = MaterialTheme.colorScheme.onPrimaryContainer,
                                                modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp)
                                            )
                                        }
                                    }
                                }
                            }
                        },
                        actions = {
                            // Search Toggle Button
                            IconButton(
                                onClick = {
                                    isSearchActive = !isSearchActive
                                    if (!isSearchActive) searchQuery = ""
                                }
                            ) {
                                Icon(
                                    imageVector = if (isSearchActive) Icons.Default.Close else Icons.Default.Search,
                                    contentDescription = "Search",
                                    tint = MaterialTheme.colorScheme.onSurface
                                )
                            }

                            // Sort Option Button with Dropdown
                            Box {
                                IconButton(onClick = { showSortMenu = true }) {
                                    Icon(
                                        imageVector = Icons.AutoMirrored.Filled.Sort,
                                        contentDescription = "Sort APKs",
                                        tint = MaterialTheme.colorScheme.onSurface
                                    )
                                }
                                DropdownMenu(
                                    expanded = showSortMenu,
                                    onDismissRequest = { showSortMenu = false }
                                ) {
                                    DropdownMenuItem(
                                        text = { Text("Alphabetical (A - Z)") },
                                        leadingIcon = { Icon(Icons.Default.SortByAlpha, contentDescription = null) },
                                        trailingIcon = if (sortMode == "alphabetical") {
                                            { Icon(Icons.Default.Check, contentDescription = null, tint = MaterialTheme.colorScheme.primary) }
                                        } else null,
                                        onClick = {
                                            sortMode = "alphabetical"
                                            prefs.edit().putString("app_sort_mode", "alphabetical").apply()
                                            showSortMenu = false
                                        }
                                    )
                                    DropdownMenuItem(
                                        text = { Text("Recently Played") },
                                        leadingIcon = { Icon(Icons.Default.History, contentDescription = null) },
                                        trailingIcon = if (sortMode == "recently_played") {
                                            { Icon(Icons.Default.Check, contentDescription = null, tint = MaterialTheme.colorScheme.primary) }
                                        } else null,
                                        onClick = {
                                            sortMode = "recently_played"
                                            prefs.edit().putString("app_sort_mode", "recently_played").apply()
                                            showSortMenu = false
                                        }
                                    )
                                    DropdownMenuItem(
                                        text = { Text("Install Date (Newest first)") },
                                        leadingIcon = { Icon(Icons.Default.Schedule, contentDescription = null) },
                                        trailingIcon = if (sortMode == "install_date") {
                                            { Icon(Icons.Default.Check, contentDescription = null, tint = MaterialTheme.colorScheme.primary) }
                                        } else null,
                                        onClick = {
                                            sortMode = "install_date"
                                            prefs.edit().putString("app_sort_mode", "install_date").apply()
                                            showSortMenu = false
                                        }
                                    )
                                }
                            }

                            // Settings Button
                            IconButton(
                                onClick = { showSettingsDialog = true }
                            ) {
                                Icon(
                                    imageVector = Icons.Default.Settings,
                                    contentDescription = "Settings",
                                    tint = MaterialTheme.colorScheme.onSurface
                                )
                            }
                        },
                        colors = TopAppBarDefaults.topAppBarColors(
                            containerColor = Color.Transparent
                        )
                    )
                }
            },
            floatingActionButton = {
                FloatingActionButton(
                    onClick = { apkPicker.launch("*/*") },
                    containerColor = MaterialTheme.colorScheme.primaryContainer,
                    contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
                    shape = RoundedCornerShape(18.dp),
                    elevation = FloatingActionButtonDefaults.elevation(defaultElevation = 6.dp)
                ) {
                    Icon(
                        imageVector = Icons.Default.Add,
                        contentDescription = "Install APK",
                        modifier = Modifier.size(26.dp)
                    )
                }
            }
        ) { paddingValues ->
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(backgroundBrush)
                    .padding(paddingValues)
            ) {
                if (installedApps.isEmpty() && !isExtracting) {
                    EmptyAppDrawerView(onInstallClick = { apkPicker.launch("*/*") }, useDarkTheme = useDarkTheme)
                } else if (filteredApps.isEmpty()) {
                    Box(
                        modifier = Modifier.fillMaxSize(),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = "No APKs match \"$searchQuery\"",
                            fontSize = 14.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                } else {
                    LazyVerticalGrid(
                        columns = GridCells.Fixed(4),
                        state = gridState,
                        modifier = Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(
                            start = 16.dp,
                            end = 16.dp,
                            top = 16.dp,
                            bottom = 96.dp
                        ),
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                        verticalArrangement = Arrangement.spacedBy(18.dp)
                    ) {
                        items(filteredApps, key = { it.packageName }) { app ->
                            AppDrawerGridItem(
                                app = app,
                                onClick = { launchApp(app) },
                                onLongClick = { selectedAppForActions = app }
                            )
                        }
                    }

                    // Dynamic Fast Scrollbar (Visible only while scrolling/dragging, hidden otherwise)
                    AnimatedVisibility(
                        visible = isScrollbarVisible && filteredApps.size > 8,
                        enter = fadeIn(),
                        exit = fadeOut(),
                        modifier = Modifier
                            .align(Alignment.CenterEnd)
                            .fillMaxHeight()
                            .padding(top = 12.dp, bottom = 96.dp)
                    ) {
                        var trackHeightPx by remember { mutableStateOf(1f) }
                        val maxScrollIndex = (filteredApps.size - 4).coerceAtLeast(1)
                        val scrollRatio = (firstVisibleIdx.toFloat() / maxScrollIndex).coerceIn(0f, 1f)

                        Box(
                            modifier = Modifier
                                .fillMaxHeight()
                                .width(36.dp)
                                .onSizeChanged { trackHeightPx = it.height.toFloat().coerceAtLeast(1f) }
                                .pointerInput(filteredApps.size) {
                                    detectVerticalDragGestures(
                                        onDragStart = { offset ->
                                            isDraggingThumb = true
                                            val ratio = (offset.y / trackHeightPx).coerceIn(0f, 1f)
                                            val targetItem = (ratio * (filteredApps.size - 1)).toInt()
                                            coroutineScope.launch { gridState.scrollToItem(targetItem) }
                                            haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                        },
                                        onDragEnd = { isDraggingThumb = false },
                                        onDragCancel = { isDraggingThumb = false },
                                        onVerticalDrag = { change, _ ->
                                            val ratio = (change.position.y / trackHeightPx).coerceIn(0f, 1f)
                                            val targetItem = (ratio * (filteredApps.size - 1)).toInt()
                                            coroutineScope.launch { gridState.scrollToItem(targetItem) }
                                            haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                        }
                                    )
                                }
                        ) {
                            val thumbHeightDp = 52.dp
                            val availableTravelPx = with(LocalDensity.current) {
                                (trackHeightPx - thumbHeightDp.toPx()).coerceAtLeast(0f)
                            }
                            val thumbOffsetPx = scrollRatio * availableTravelPx

                            // Scrollbar Thumb Pill
                            Box(
                                modifier = Modifier
                                    .align(Alignment.TopEnd)
                                    .offset(
                                        x = (-4).dp,
                                        y = with(LocalDensity.current) { thumbOffsetPx.toDp() }
                                    )
                                    .width(if (isDraggingThumb) 6.dp else 4.dp)
                                    .height(thumbHeightDp)
                                    .clip(CircleShape)
                                    .background(
                                        if (isDraggingThumb) MaterialTheme.colorScheme.primary
                                        else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
                                    )
                            )

                            // Floating letter bubble (active only while scrolling / dragging if alphabetical)
                            if (activeSectionLetter != null) {
                                Box(
                                    modifier = Modifier
                                        .align(Alignment.TopEnd)
                                        .offset(
                                            x = (-26).dp,
                                            y = with(LocalDensity.current) {
                                                (thumbOffsetPx - 6f).toDp().coerceAtLeast(0.dp)
                                            }
                                        )
                                ) {
                                    Surface(
                                        shape = CircleShape,
                                        color = MaterialTheme.colorScheme.primaryContainer,
                                        shadowElevation = 8.dp,
                                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.primary.copy(alpha = 0.35f)),
                                        modifier = Modifier.size(46.dp)
                                    ) {
                                        Box(contentAlignment = Alignment.Center) {
                                            Text(
                                                text = "$activeSectionLetter",
                                                fontSize = 22.sp,
                                                fontWeight = FontWeight.Bold,
                                                color = MaterialTheme.colorScheme.onPrimaryContainer
                                            )
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // APK Ingestion / Extraction Progress Overlay
                if (isExtracting) {
                    Box(
                        modifier = Modifier
                            .fillMaxSize()
                            .background(Color.Black.copy(alpha = 0.75f)),
                        contentAlignment = Alignment.Center
                    ) {
                        Surface(
                            shape = RoundedCornerShape(20.dp),
                            color = if (useDarkTheme) Color(0xEE161922) else Color(0xF2FFFFFF),
                            border = BorderStroke(1.dp, Color.White.copy(alpha = 0.1f)),
                            shadowElevation = 12.dp
                        ) {
                            Column(
                                modifier = Modifier.padding(28.dp),
                                horizontalAlignment = Alignment.CenterHorizontally
                            ) {
                                CircularProgressIndicator(
                                    color = MaterialTheme.colorScheme.primary,
                                    strokeWidth = 3.dp
                                )
                                Spacer(modifier = Modifier.height(18.dp))
                                Text(
                                    text = extractingName,
                                    fontSize = 14.sp,
                                    fontWeight = FontWeight.Medium,
                                    color = MaterialTheme.colorScheme.onSurface
                                )
                            }
                        }
                    }
                }
            }
        }

        // Action Sheet on Long-Press
        selectedAppForActions?.let { app ->
            val inAppDrawer = remember(globalRefreshTrigger, refreshTrigger, app.packageName) {
                ContainerRegistry.isAppInAppDrawer(this@MainActivity, app.packageName)
            }

            ModalBottomSheet(
                onDismissRequest = { selectedAppForActions = null },
                sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
                containerColor = if (useDarkTheme) Color(0xF212151E) else Color(0xF2FFFFFF),
                tonalElevation = 6.dp,
                shape = RoundedCornerShape(topStart = 24.dp, topEnd = 24.dp),
                dragHandle = { BottomSheetDefaults.DragHandle() }
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 20.dp)
                        .padding(bottom = 32.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    // App preview header
                    AppSheetHeader(app = app)

                    HorizontalDivider(
                        modifier = Modifier.padding(vertical = 10.dp),
                        color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.4f)
                    )

                    // 1. Launch
                    ActionRowItem(
                        icon = Icons.Default.PlayArrow,
                        iconTint = MaterialTheme.colorScheme.primary,
                        title = "Launch",
                        subtitle = "Start 32-bit container",
                        onClick = {
                            selectedAppForActions = null
                            launchApp(app)
                        }
                    )

                    // 2. More Info
                    ActionRowItem(
                        icon = Icons.Default.Info,
                        iconTint = MaterialTheme.colorScheme.secondary,
                        title = "More Info",
                        subtitle = "Package, libraries, per-app settings & container storage",
                        onClick = {
                            selectedAppForActions = null
                            selectedAppForInfo = app
                        }
                    )

                    // 3. Install / Remove from Apps
                    ActionRowItem(
                        icon = Icons.Default.Apps,
                        iconTint = if (inAppDrawer) Color(0xFFE57373) else MaterialTheme.colorScheme.tertiary,
                        title = if (inAppDrawer) "Remove from Apps" else "Add to Apps",
                        subtitle = if (inAppDrawer) "Remove stub from phone app drawer" else "Launch directly from phone app drawer",
                        onClick = {
                            selectedAppForActions = null
                            if (inAppDrawer) {
                                appRemovingStub = app
                                val intent = StubApkGenerator.createUninstallIntent(this@MainActivity, app.packageName)
                                if (intent != null) {
                                    try {
                                        stubUninstallLauncher.launch(intent)
                                    } catch (e: Throwable) {
                                        Log.w(TAG, "Failed to launch stub uninstall launcher: ${e.message}")
                                        StubApkGenerator.uninstallStub(this@MainActivity, app.packageName)
                                    }
                                } else {
                                    StubApkGenerator.uninstallStub(this@MainActivity, app.packageName)
                                }
                            } else {
                                thread {
                                    ContainerRegistry.installToAppDrawer(this@MainActivity, app.packageName)
                                    runOnUiThread {
                                        refreshTrigger++
                                        globalRefreshTrigger++
                                        Toast.makeText(this@MainActivity, "Adding ${app.appName} to apps...", Toast.LENGTH_SHORT).show()
                                    }
                                }
                            }
                        }
                    )

                    // 4. Expose App Data
                    ActionRowItem(
                        icon = Icons.Default.FolderOpen,
                        iconTint = if (app.isDataExposed) Color(0xFF81C784) else MaterialTheme.colorScheme.outline,
                        title = "Expose App Data",
                        subtitle = if (app.isDataExposed) "Exposed: /sdcard/vemod/${ContainerRegistry.getExposedFolderRelativePath(this@MainActivity, app)}" else "Isolated in private container sandbox",
                        trailing = {
                            Switch(
                                checked = app.isDataExposed,
                                onCheckedChange = { checked ->
                                    selectedAppForActions = null
                                    if (checked && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
                                        try {
                                            val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).apply {
                                                data = Uri.parse("package:$packageName")
                                            }
                                            manageStorageLauncher.launch(intent)
                                        } catch (_: Throwable) {
                                            val intent = Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
                                            manageStorageLauncher.launch(intent)
                                        }
                                    } else {
                                        thread {
                                            ContainerRegistry.setExposeAppData(this@MainActivity, app.packageName, checked)
                                            runOnUiThread { refreshTrigger++ }
                                        }
                                    }
                                }
                            )
                        },
                        onClick = {
                            selectedAppForActions = null
                            val newChecked = !app.isDataExposed
                            if (newChecked && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
                                try {
                                    val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).apply {
                                        data = Uri.parse("package:$packageName")
                                    }
                                    manageStorageLauncher.launch(intent)
                                } catch (_: Throwable) {
                                    val intent = Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
                                    manageStorageLauncher.launch(intent)
                                }
                            } else {
                                thread {
                                    ContainerRegistry.setExposeAppData(this@MainActivity, app.packageName, newChecked)
                                    runOnUiThread { refreshTrigger++ }
                                }
                            }
                        }
                    )

                    // 5. Uninstall
                    ActionRowItem(
                        icon = Icons.Default.Delete,
                        iconTint = MaterialTheme.colorScheme.error,
                        title = "Uninstall",
                        subtitle = "Delete container files and companion stub",
                        onClick = {
                            selectedAppForActions = null
                            appToUninstall = app
                        }
                    )
                }
            }
        }

        // Uninstall Confirmation Dialog
        appToUninstall?.let { app ->
            AlertDialog(
                onDismissRequest = { appToUninstall = null },
                title = { Text("Uninstall ${app.appName}?", fontWeight = FontWeight.Bold) },
                text = {
                    Text(
                        "This will permanently delete the 32-bit container, app data, native libraries, and unregister its companion launcher stub.",
                        fontSize = 13.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                },
                confirmButton = {
                    Button(
                        onClick = {
                            val target = app
                            appToUninstall = null
                            if (ContainerRegistry.isAppInAppDrawer(this@MainActivity, target.packageName)) {
                                val uninstallIntent = StubApkGenerator.createUninstallIntent(this@MainActivity, target.packageName)
                                if (uninstallIntent != null) {
                                    try {
                                        stubUninstallLauncher.launch(uninstallIntent)
                                    } catch (e: Throwable) {
                                        Log.w(TAG, "Failed to launch stub uninstall launcher: ${e.message}")
                                        StubApkGenerator.uninstallStub(this@MainActivity, target.packageName)
                                    }
                                } else {
                                    StubApkGenerator.uninstallStub(this@MainActivity, target.packageName)
                                }
                            }
                            thread {
                                ContainerRegistry.uninstallApp(this@MainActivity, target.packageName)
                                runOnUiThread {
                                    refreshTrigger++
                                    globalRefreshTrigger++
                                    Toast.makeText(this@MainActivity, "Uninstalled ${target.appName}", Toast.LENGTH_SHORT).show()
                                }
                            }
                        },
                        colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)
                    ) {
                        Text("Uninstall")
                    }
                },
                dismissButton = {
                    TextButton(onClick = { appToUninstall = null }) {
                        Text("Cancel")
                    }
                }
            )
        }

        // More Info Dialog
        selectedAppForInfo?.let { app ->
            MoreInfoDialog(
                app = app,
                useDarkTheme = useDarkTheme,
                onDismiss = { selectedAppForInfo = null }
            )
        }

        // Streamlined Settings Dialog
        if (showSettingsDialog) {
            StreamlinedSettingsDialog(
                useDarkTheme = useDarkTheme,
                themeMode = themeMode,
                onThemeChange = onThemeChange,
                currentLogLevel = currentLogLevel,
                onLogLevelChange = { level ->
                    currentLogLevel = level
                    prefs.edit().putInt("log_level", level).apply()
                    VemodBridge.nativeSetLogLevel(level)
                },
                pipEnabled = pipEnabled,
                onPipChange = { enabled ->
                    pipEnabled = enabled
                    prefs.edit().putBoolean("pip_enabled", enabled).apply()
                },
                onDismiss = { showSettingsDialog = false }
            )
        }
    }

    @OptIn(ExperimentalFoundationApi::class)
    @Composable
    fun AppDrawerGridItem(
        app: ContainerApp,
        onClick: () -> Unit,
        onLongClick: () -> Unit
    ) {
        val haptic = LocalHapticFeedback.current
        val iconBitmap = remember(app.iconFile?.absolutePath) {
            if (app.iconFile != null && app.iconFile.exists()) {
                try {
                    BitmapFactory.decodeFile(app.iconFile.absolutePath)?.asImageBitmap()
                } catch (_: Throwable) {
                    null
                }
            } else null
        }

        Column(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(14.dp))
                .combinedClickable(
                    onClick = onClick,
                    onLongClick = {
                        haptic.performHapticFeedback(HapticFeedbackType.LongPress)
                        onLongClick()
                    }
                )
                .padding(vertical = 8.dp, horizontal = 4.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // App Icon
            if (iconBitmap != null) {
                Image(
                    bitmap = iconBitmap,
                    contentDescription = app.appName,
                    modifier = Modifier
                        .size(56.dp)
                        .clip(RoundedCornerShape(14.dp))
                )
            } else {
                Surface(
                    modifier = Modifier
                        .size(56.dp)
                        .clip(RoundedCornerShape(14.dp)),
                    color = MaterialTheme.colorScheme.primaryContainer,
                    tonalElevation = 3.dp
                ) {
                    Box(contentAlignment = Alignment.Center) {
                        Text(
                            text = app.appName.take(1).uppercase(),
                            fontSize = 24.sp,
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.onPrimaryContainer
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.height(6.dp))

            // Label only
            Text(
                text = app.appName,
                fontSize = 11.5.sp,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
                lineHeight = 14.sp,
                modifier = Modifier.fillMaxWidth()
            )
        }
    }

    @Composable
    fun AppSheetHeader(app: ContainerApp) {
        val iconBitmap = remember(app.iconFile?.absolutePath) {
            if (app.iconFile != null && app.iconFile.exists()) {
                try {
                    BitmapFactory.decodeFile(app.iconFile.absolutePath)?.asImageBitmap()
                } catch (_: Throwable) {
                    null
                }
            } else null
        }

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            if (iconBitmap != null) {
                Image(
                    bitmap = iconBitmap,
                    contentDescription = app.appName,
                    modifier = Modifier
                        .size(48.dp)
                        .clip(RoundedCornerShape(12.dp))
                )
            } else {
                Surface(
                    modifier = Modifier
                        .size(48.dp)
                        .clip(RoundedCornerShape(12.dp)),
                    color = MaterialTheme.colorScheme.primaryContainer
                ) {
                    Box(contentAlignment = Alignment.Center) {
                        Text(
                            text = app.appName.take(1).uppercase(),
                            fontSize = 20.sp,
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.onPrimaryContainer
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.width(14.dp))

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
                    text = "${app.packageName} • v${app.versionName}",
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
    }

    @Composable
    fun ActionRowItem(
        icon: androidx.compose.ui.graphics.vector.ImageVector,
        iconTint: Color,
        title: String,
        subtitle: String,
        trailing: (@Composable () -> Unit)? = null,
        onClick: () -> Unit
    ) {
        Surface(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(12.dp))
                .clickable(onClick = onClick),
            color = Color.Transparent
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 10.dp, horizontal = 6.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Surface(
                    shape = RoundedCornerShape(10.dp),
                    color = iconTint.copy(alpha = 0.12f),
                    modifier = Modifier.size(38.dp)
                ) {
                    Box(contentAlignment = Alignment.Center) {
                        Icon(
                            imageVector = icon,
                            contentDescription = title,
                            tint = iconTint,
                            modifier = Modifier.size(20.dp)
                        )
                    }
                }

                Spacer(modifier = Modifier.width(14.dp))

                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = title,
                        fontSize = 14.sp,
                        fontWeight = FontWeight.SemiBold,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    Text(
                        text = subtitle,
                        fontSize = 11.5.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }

                if (trailing != null) {
                    Spacer(modifier = Modifier.width(8.dp))
                    trailing()
                }
            }
        }
    }

    @Composable
    fun MoreInfoDialog(
        app: ContainerApp,
        useDarkTheme: Boolean,
        onDismiss: () -> Unit
    ) {
        val context = LocalContext.current
        val iconBitmap = remember(app.iconFile?.absolutePath) {
            if (app.iconFile != null && app.iconFile.exists()) {
                try {
                    BitmapFactory.decodeFile(app.iconFile.absolutePath)?.asImageBitmap()
                } catch (_: Throwable) {
                    null
                }
            } else null
        }

        val containerSizeStr = remember(app.containerDir.absolutePath) {
            formatFileSize(calculateDirSize(app.containerDir))
        }

        val inAppDrawer = remember(globalRefreshTrigger, app.packageName) {
            ContainerRegistry.isAppInAppDrawer(context, app.packageName)
        }

        val lastPlayedTime = remember(globalRefreshTrigger, app.packageName) {
            ContainerRegistry.getLastLaunchTime(context, app.packageName)
        }

        var appPipEnabled by remember(app.packageName) {
            mutableStateOf(ContainerRegistry.isPipEnabledForApp(context, app.packageName))
        }

        Dialog(
            onDismissRequest = onDismiss,
            properties = DialogProperties(usePlatformDefaultWidth = false)
        ) {
            Surface(
                modifier = Modifier
                    .fillMaxWidth(0.92f)
                    .fillMaxHeight(0.88f),
                shape = RoundedCornerShape(24.dp),
                color = if (useDarkTheme) Color(0xF2141722) else Color(0xF2FFFFFF),
                border = BorderStroke(1.dp, if (useDarkTheme) Color.White.copy(alpha = 0.08f) else Color.Black.copy(alpha = 0.06f)),
                shadowElevation = 16.dp
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(20.dp)
                ) {
                    // Header
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        if (iconBitmap != null) {
                            Image(
                                bitmap = iconBitmap,
                                contentDescription = app.appName,
                                modifier = Modifier
                                    .size(52.dp)
                                    .clip(RoundedCornerShape(14.dp))
                            )
                        } else {
                            Surface(
                                modifier = Modifier
                                    .size(52.dp)
                                    .clip(RoundedCornerShape(14.dp)),
                                color = MaterialTheme.colorScheme.primaryContainer
                            ) {
                                Box(contentAlignment = Alignment.Center) {
                                    Text(
                                        text = app.appName.take(1).uppercase(),
                                        fontSize = 22.sp,
                                        fontWeight = FontWeight.Bold,
                                        color = MaterialTheme.colorScheme.onPrimaryContainer
                                    )
                                }
                            }
                        }

                        Spacer(modifier = Modifier.width(14.dp))

                        Column(modifier = Modifier.weight(1f)) {
                            Text(
                                text = app.appName,
                                fontSize = 18.sp,
                                fontWeight = FontWeight.Bold,
                                color = MaterialTheme.colorScheme.onSurface
                            )
                            Text(
                                text = "v${app.versionName} (Build ${app.versionCode})",
                                fontSize = 12.sp,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }

                        IconButton(onClick = onDismiss) {
                            Icon(Icons.Default.Close, contentDescription = "Close")
                        }
                    }

                    HorizontalDivider(
                        modifier = Modifier.padding(vertical = 12.dp),
                        color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.4f)
                    )

                    // Scrollable Info Content
                    Column(
                        modifier = Modifier
                            .weight(1f)
                            .verticalScroll(rememberScrollState()),
                        verticalArrangement = Arrangement.spacedBy(14.dp)
                    ) {
                        // Section: Per-App Configuration
                        InfoSectionTitle("Per-App Configuration")
                        Surface(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clip(RoundedCornerShape(12.dp))
                                .clickable {
                                    val newPip = !appPipEnabled
                                    appPipEnabled = newPip
                                    ContainerRegistry.setPipEnabledForApp(context, app.packageName, newPip)
                                    Toast.makeText(context, if (newPip) "PiP enabled for ${app.appName}" else "PiP disabled for ${app.appName}", Toast.LENGTH_SHORT).show()
                                },
                            color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.4f),
                            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.15f))
                        ) {
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(horizontal = 12.dp, vertical = 10.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.SpaceBetween
                            ) {
                                Column(modifier = Modifier.weight(1f)) {
                                    Text(
                                        text = "Picture-in-Picture (PiP)",
                                        fontSize = 13.5.sp,
                                        fontWeight = FontWeight.SemiBold,
                                        color = MaterialTheme.colorScheme.onSurface
                                    )
                                    Text(
                                        text = "Allow this game to enter PiP mode when navigating to home or other apps",
                                        fontSize = 11.5.sp,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant
                                    )
                                }
                                Spacer(modifier = Modifier.width(12.dp))
                                Switch(
                                    checked = appPipEnabled,
                                    onCheckedChange = { checked ->
                                        appPipEnabled = checked
                                        ContainerRegistry.setPipEnabledForApp(context, app.packageName, checked)
                                        Toast.makeText(context, if (checked) "PiP enabled for ${app.appName}" else "PiP disabled for ${app.appName}", Toast.LENGTH_SHORT).show()
                                    }
                                )
                            }
                        }

                        // Section: Package & Component
                        InfoSectionTitle("Application Identifiers")
                        InfoRow(
                            label = "Package Name",
                            value = app.packageName,
                            canCopy = true,
                            context = context
                        )
                        InfoRow(
                            label = "Launch Activity",
                            value = app.launchActivity
                        )
                        if (app.applicationClass != null) {
                            InfoRow(
                                label = "Application Class",
                                value = app.applicationClass
                            )
                        }

                        // Section: 32-Bit Subsystem & Native Libraries
                        InfoSectionTitle("32-Bit ARM Runtime")
                        InfoRow(
                            label = "Architecture",
                            value = "armeabi-v7a (32-Bit ARM Dynarmic JIT)"
                        )
                        InfoRow(
                            label = "Orientation",
                            value = formatOrientation(app.screenOrientation)
                        )
                        InfoRow(
                            label = "Native Libraries (${app.nativeLibraries.size})",
                            value = if (app.nativeLibraries.isEmpty()) "None (Pure Dalvik bytecode)" else app.nativeLibraries.joinToString(", ")
                        )

                        // Section: Storage & Isolation
                        InfoSectionTitle("Storage & Sandbox")
                        InfoRow(
                            label = "Container Footprint",
                            value = containerSizeStr
                        )
                        InfoRow(
                            label = "Data Isolation",
                            value = if (app.isDataExposed) "Exposed to Shared Storage" else "Isolated in Container Sandbox"
                        )
                        if (app.isDataExposed) {
                            val folder = ContainerRegistry.getExposedFolderRelativePath(context, app)
                            InfoRow(
                                label = "Shared Folder",
                                value = "/sdcard/vemod/$folder",
                                canCopy = true,
                                context = context
                            )
                        }
                        InfoRow(
                            label = "Container Path",
                            value = app.containerDir.absolutePath,
                            canCopy = true,
                            context = context
                        )

                        // Section: Integration & History
                        InfoSectionTitle("System Integration & Usage")
                        InfoRow(
                            label = "Phone App Drawer",
                            value = if (inAppDrawer) "Installed (com.vemod.container_${app.packageName})" else "Not Installed"
                        )
                        InfoRow(
                            label = "Recently Played",
                            value = if (lastPlayedTime > 0) formatDate(lastPlayedTime) else "Never launched"
                        )
                        InfoRow(
                            label = "Installed At",
                            value = formatDate(app.installedAt)
                        )
                    }

                    Spacer(modifier = Modifier.height(12.dp))

                    Button(
                        onClick = onDismiss,
                        shape = RoundedCornerShape(12.dp),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("Done")
                    }
                }
            }
        }
    }

    @Composable
    fun InfoSectionTitle(title: String) {
        Text(
            text = title,
            fontSize = 12.sp,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.padding(top = 4.dp, bottom = 2.dp)
        )
    }

    @Composable
    fun InfoRow(
        label: String,
        value: String,
        canCopy: Boolean = false,
        context: Context? = null
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .clickable(enabled = canCopy) {
                    if (canCopy && context != null) {
                        val clip = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                        clip.setPrimaryClip(ClipData.newPlainText(label, value))
                        Toast.makeText(context, "Copied $label", Toast.LENGTH_SHORT).show()
                    }
                }
                .padding(vertical = 4.dp, horizontal = 4.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = label,
                    fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                if (canCopy) {
                    Icon(
                        imageVector = Icons.Default.ContentCopy,
                        contentDescription = "Copy",
                        tint = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.size(14.dp)
                    )
                }
            }
            Spacer(modifier = Modifier.height(2.dp))
            Text(
                text = value,
                fontSize = 13.sp,
                fontFamily = if (value.contains(".") || value.contains("/")) FontFamily.Monospace else FontFamily.Default,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.onSurface
            )
        }
    }

    @Composable
    fun StreamlinedSettingsDialog(
        useDarkTheme: Boolean,
        themeMode: String,
        onThemeChange: (String) -> Unit,
        currentLogLevel: Int,
        onLogLevelChange: (Int) -> Unit,
        pipEnabled: Boolean,
        onPipChange: (Boolean) -> Unit,
        onDismiss: () -> Unit
    ) {
        val logOptions = listOf(
            Triple(VemodBridge.LOG_LEVEL_NONE, "No logging", "Maximum performance - disables bridge logs"),
            Triple(VemodBridge.LOG_LEVEL_BASIC, "Basic logs", "Default - lifecycle events and errors"),
            Triple(VemodBridge.LOG_LEVEL_ADVANCED, "Advanced logging", "Sampled JNI dispatch & thread lifecycle"),
            Triple(VemodBridge.LOG_LEVEL_FULL, "Full logging", "All JNI calls, raw syscalls & detailed tracing")
        )
        val selectedLogOption = logOptions.find { it.first == currentLogLevel } ?: logOptions[1]
        var logDropdownExpanded by remember { mutableStateOf(false) }

        Dialog(
            onDismissRequest = onDismiss,
            properties = DialogProperties(usePlatformDefaultWidth = false)
        ) {
            Surface(
                modifier = Modifier
                    .fillMaxWidth(0.92f)
                    .wrapContentHeight(),
                shape = RoundedCornerShape(24.dp),
                color = if (useDarkTheme) Color(0xF2141722) else Color(0xF2FFFFFF),
                border = BorderStroke(1.dp, if (useDarkTheme) Color.White.copy(alpha = 0.08f) else Color.Black.copy(alpha = 0.06f)),
                shadowElevation = 16.dp
            ) {
                Column(
                    modifier = Modifier.padding(24.dp),
                    verticalArrangement = Arrangement.spacedBy(16.dp)
                ) {
                    // Header
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = "Settings",
                            fontWeight = FontWeight.Bold,
                            fontSize = 20.sp,
                            color = MaterialTheme.colorScheme.onSurface
                        )
                        IconButton(onClick = onDismiss) {
                            Icon(Icons.Default.Close, contentDescription = "Close")
                        }
                    }

                    // 1. Theme Option: System / Dark / Light
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(
                            text = "Appearance",
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.primary
                        )
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            listOf(
                                Triple("system", "System", Icons.Default.BrightnessAuto),
                                Triple("dark", "Dark", Icons.Default.DarkMode),
                                Triple("light", "Light", Icons.Default.LightMode)
                            ).forEach { (mode, label, icon) ->
                                val isSelected = themeMode == mode
                                Surface(
                                    modifier = Modifier
                                        .weight(1f)
                                        .clip(RoundedCornerShape(12.dp))
                                        .clickable { onThemeChange(mode) },
                                    shape = RoundedCornerShape(12.dp),
                                    color = if (isSelected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f),
                                    border = if (isSelected) BorderStroke(1.5.dp, MaterialTheme.colorScheme.primary) else BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.15f))
                                ) {
                                    Column(
                                        modifier = Modifier.padding(vertical = 10.dp),
                                        horizontalAlignment = Alignment.CenterHorizontally
                                    ) {
                                        Icon(
                                            imageVector = icon,
                                            contentDescription = label,
                                            tint = if (isSelected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                                            modifier = Modifier.size(20.dp)
                                        )
                                        Spacer(modifier = Modifier.height(4.dp))
                                        Text(
                                            text = label,
                                            fontSize = 11.5.sp,
                                            fontWeight = if (isSelected) FontWeight.Bold else FontWeight.Normal,
                                            color = if (isSelected) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant
                                        )
                                    }
                                }
                            }
                        }
                    }

                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.3f))

                    // 2. Streamlined Debug Logs (Single Setting with Dropdown)
                    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text(
                            text = "Debug Logs",
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.primary
                        )
                        Text(
                            text = "Configure native bridge & guest logging overhead",
                            fontSize = 11.5.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                        Spacer(modifier = Modifier.height(2.dp))

                        Box {
                            Surface(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .clip(RoundedCornerShape(12.dp))
                                    .clickable { logDropdownExpanded = true },
                                shape = RoundedCornerShape(12.dp),
                                color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f),
                                border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.2f))
                            ) {
                                Row(
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .padding(horizontal = 14.dp, vertical = 12.dp),
                                    horizontalArrangement = Arrangement.SpaceBetween,
                                    verticalAlignment = Alignment.CenterVertically
                                ) {
                                    Column(modifier = Modifier.weight(1f)) {
                                        Text(
                                            text = selectedLogOption.second,
                                            fontSize = 14.sp,
                                            fontWeight = FontWeight.Medium,
                                            color = MaterialTheme.colorScheme.onSurface
                                        )
                                        Text(
                                            text = selectedLogOption.third,
                                            fontSize = 11.sp,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                            maxLines = 1,
                                            overflow = TextOverflow.Ellipsis
                                        )
                                    }
                                    Icon(
                                        imageVector = Icons.Default.ArrowDropDown,
                                        contentDescription = "Select Log Level",
                                        tint = MaterialTheme.colorScheme.onSurfaceVariant
                                    )
                                }
                            }

                            DropdownMenu(
                                expanded = logDropdownExpanded,
                                onDismissRequest = { logDropdownExpanded = false }
                            ) {
                                logOptions.forEach { (level, title, desc) ->
                                    DropdownMenuItem(
                                        text = {
                                            Column {
                                                Text(
                                                    text = title,
                                                    fontWeight = if (level == currentLogLevel) FontWeight.Bold else FontWeight.Medium,
                                                    fontSize = 14.sp
                                                )
                                                Text(
                                                    text = desc,
                                                    fontSize = 11.sp,
                                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                                )
                                            }
                                        },
                                        onClick = {
                                            onLogLevelChange(level)
                                            logDropdownExpanded = false
                                        },
                                        leadingIcon = if (level == currentLogLevel) {
                                            { Icon(Icons.Default.Check, contentDescription = null, tint = MaterialTheme.colorScheme.primary) }
                                        } else null
                                    )
                                }
                            }
                        }
                    }

                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.3f))

                    // 3. Picture-in-Picture (Global Default)
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(12.dp))
                            .clickable { onPipChange(!pipEnabled) }
                            .padding(vertical = 4.dp),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(
                                text = "Picture-in-Picture (Global Default)",
                                fontWeight = FontWeight.SemiBold,
                                fontSize = 14.sp,
                                color = MaterialTheme.colorScheme.onSurface
                            )
                            Text(
                                text = "Default PiP policy for apps unless customized in More Info",
                                fontSize = 11.5.sp,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                        Switch(
                            checked = pipEnabled,
                            onCheckedChange = onPipChange
                        )
                    }

                    Spacer(modifier = Modifier.height(4.dp))

                    Button(
                        onClick = onDismiss,
                        shape = RoundedCornerShape(12.dp),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("Close")
                    }
                }
            }
        }
    }

    @Composable
    fun EmptyAppDrawerView(
        onInstallClick: () -> Unit,
        useDarkTheme: Boolean
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(28.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Surface(
                modifier = Modifier
                    .fillMaxWidth(0.9f)
                    .clip(RoundedCornerShape(24.dp)),
                shape = RoundedCornerShape(24.dp),
                color = if (useDarkTheme) Color(0xCC131620) else Color(0xEEFFFFFF),
                border = BorderStroke(1.dp, if (useDarkTheme) Color.White.copy(alpha = 0.08f) else Color.Black.copy(alpha = 0.06f)),
                shadowElevation = 8.dp
            ) {
                Column(
                    modifier = Modifier.padding(28.dp),
                    horizontalAlignment = Alignment.CenterHorizontally
                ) {
                    Surface(
                        shape = CircleShape,
                        color = MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.8f),
                        modifier = Modifier.size(68.dp)
                    ) {
                        Box(contentAlignment = Alignment.Center) {
                            Icon(
                                imageVector = Icons.Default.Apps,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.onPrimaryContainer,
                                modifier = Modifier.size(36.dp)
                            )
                        }
                    }

                    Spacer(modifier = Modifier.height(18.dp))

                    Text(
                        text = "No APKs Installed",
                        fontSize = 19.sp,
                        fontWeight = FontWeight.Bold,
                        color = MaterialTheme.colorScheme.onSurface
                    )

                    Spacer(modifier = Modifier.height(8.dp))

                    Text(
                        text = "Tap the + button below or install legacy 32-bit (armeabi-v7a) APKs from Downloads or file managers to run them inside the isolated Dynarmic container.",
                        fontSize = 12.5.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        lineHeight = 17.sp,
                        textAlign = TextAlign.Center
                    )

                    Spacer(modifier = Modifier.height(20.dp))

                    Button(
                        onClick = onInstallClick,
                        shape = RoundedCornerShape(14.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.primary),
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(44.dp)
                    ) {
                        Icon(Icons.Default.Add, contentDescription = null, modifier = Modifier.size(18.dp))
                        Spacer(modifier = Modifier.width(8.dp))
                        Text("Pick APK", fontSize = 13.5.sp)
                    }
                }
            }
        }
    }

    private fun formatOrientation(orientation: Int): String {
        return when (orientation) {
            0, 6, 8, 11 -> "Landscape ($orientation)"
            1, 7, 9, 12 -> "Portrait ($orientation)"
            -1 -> "Unspecified / Auto (-1)"
            2, 4, 10 -> "Sensor ($orientation)"
            else -> "Default ($orientation)"
        }
    }

    private fun formatFileSize(bytes: Long): String {
        if (bytes <= 0) return "0 B"
        val units = arrayOf("B", "KB", "MB", "GB")
        val digitGroups = (Math.log10(bytes.toDouble()) / Math.log10(1024.0)).toInt().coerceIn(0, 3)
        val formatted = String.format(Locale.US, "%.1f", bytes / Math.pow(1024.0, digitGroups.toDouble()))
        return "$formatted ${units[digitGroups]}"
    }

    private fun calculateDirSize(dir: File): Long {
        var total = 0L
        try {
            dir.walkTopDown().forEach { file ->
                if (file.isFile) total += file.length()
            }
        } catch (_: Throwable) {}
        return total
    }

    private fun formatDate(timestamp: Long): String {
        if (timestamp <= 0) return "Unknown"
        val sdf = SimpleDateFormat("MMM dd, yyyy HH:mm", Locale.getDefault())
        return sdf.format(Date(timestamp))
    }

    private fun launchApp(app: ContainerApp) {
        ContainerRegistry.recordAppLaunch(this, app.packageName)
        globalRefreshTrigger++

        val stubPkg = StubApkGenerator.getStubPackageName(app.packageName)
        if (StubApkGenerator.isStubInstalled(this, app.packageName)) {
            val stubIntent = packageManager.getLaunchIntentForPackage(stubPkg)
            if (stubIntent != null) {
                stubIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                startActivity(stubIntent)
                return
            }
        }

        val intent = Intent().apply {
            setClassName(this@MainActivity, GuestProxyActivity::class.java.name)
            putExtra(GuestProxyActivity.EXTRA_CONTAINER_DIR, app.containerDir.absolutePath)
            putExtra(GuestProxyActivity.EXTRA_BASE_APK, app.baseApk.absolutePath)
            putExtra(GuestProxyActivity.EXTRA_PACKAGE_NAME, app.packageName)
            putExtra(GuestProxyActivity.EXTRA_LAUNCH_ACTIVITY, app.launchActivity)
            putExtra(GuestProxyActivity.EXTRA_SCREEN_ORIENTATION, app.screenOrientation)
            putExtra(GuestProxyActivity.EXTRA_IS_DATA_EXPOSED, app.isDataExposed)
            if (app.isDataExposed) {
                val expDir = ContainerRegistry.getExposedDir(this@MainActivity, app)
                putExtra(GuestProxyActivity.EXTRA_EXPOSED_DIR, expDir.absolutePath)
            }
            flags = Intent.FLAG_ACTIVITY_NEW_TASK
        }
        startActivity(intent)
    }
}
