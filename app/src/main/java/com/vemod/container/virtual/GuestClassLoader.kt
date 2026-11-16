package com.vemod.container.virtual

import android.util.Log
import dalvik.system.PathClassLoader
import java.io.File

class GuestClassLoader(
    dexPath: String,
    private val proxyDir: File,
    parent: ClassLoader
) : PathClassLoader(dexPath, proxyDir.absolutePath, parent) {

    private val hostLoader = parent
    private val httpLoader: ClassLoader? = try {
        val f = File("/system/framework/org.apache.http.legacy.jar")
        if (f.exists()) PathClassLoader(f.absolutePath, parent) else null
    } catch (_: Throwable) { null }

    override fun loadClass(name: String, resolve: Boolean): Class<*> {
        // Force Vemod bridge classes, Rovio FileReader, and FuseAudio hook to resolve from the host loader
        if (name.startsWith("com.vemod.container.bridge.") || name == "com.rovio.fusion.FileReader" || name == "com.polarbit.fuse.FuseAudio") {
            return hostLoader.loadClass(name)
        }
        if (name.startsWith("org.apache.http.") && httpLoader != null) {
            try {
                return httpLoader.loadClass(name)
            } catch (_: Throwable) {}
        }
        return super.loadClass(name, resolve)
    }

    override fun findLibrary(name: String): String? {
        val fileName = if (name.startsWith("lib") && name.endsWith(".so")) name else "lib$name.so"
        val proxyFile = File(proxyDir, fileName)
        if (proxyFile.exists()) {
            Log.i("GuestClassLoader", "findLibrary($name) redirected to proxy: ${proxyFile.absolutePath}")
            return proxyFile.absolutePath
        }
        if (name == "main") {
            val unityProxy = File(proxyDir, "libunity.so")
            if (unityProxy.exists()) {
                Log.i("GuestClassLoader", "findLibrary(main) redirected to unity proxy: ${unityProxy.absolutePath}")
                return unityProxy.absolutePath
            }
        }
        Log.w("GuestClassLoader", "findLibrary($name): proxy not found at ${proxyFile.absolutePath}")
        return super.findLibrary(name)
    }
}
