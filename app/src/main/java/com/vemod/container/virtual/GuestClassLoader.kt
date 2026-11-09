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

    override fun loadClass(name: String, resolve: Boolean): Class<*> {
        // Force Vemod bridge classes to resolve from the host loader
        if (name.startsWith("com.vemod.container.bridge.")) {
            return hostLoader.loadClass(name)
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
        Log.w("GuestClassLoader", "findLibrary($name): proxy not found at ${proxyFile.absolutePath}")
        return super.findLibrary(name)
    }
}
