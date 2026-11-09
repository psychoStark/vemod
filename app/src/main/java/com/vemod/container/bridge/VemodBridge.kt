package com.vemod.container.bridge

import android.util.Log
import java.io.File
import java.lang.reflect.Method
import java.lang.reflect.Modifier

object VemodBridge {
    private const val TAG = "VemodBridge"

    init {
        try {
            System.loadLibrary("bridge")
            Log.i(TAG, "libbridge.so loaded successfully")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "Failed to load libbridge.so", e)
        }
    }

    external fun nativeInit(filesDir: String)
    external fun nativeLoadLibrary(libPath: String): Boolean
    external fun nativeGetExports(): Array<String>
    external fun nativeGetGuestSymbol(symName: String): Long
    external fun nativeCallJniOnLoad(): Int
    external fun nativeRegisterMethod(
        targetClass: Class<*>,
        methodName: String,
        methodSig: String,
        guestFn: Long
    ): Boolean

    private var activeClassLoader: ClassLoader? = null
    private var pluginDir: File? = null

    @JvmStatic
    fun setActivePlugin(dir: File, classLoader: ClassLoader) {
        pluginDir = dir
        activeClassLoader = classLoader
        nativeInit(dir.absolutePath)
    }

    /**
     * Called by libproxy.so's JNI_OnLoad via dladdr lookup of its own file path.
     */
    @JvmStatic
    fun onProxyLoaded(proxyPath: String): Int {
        Log.i(TAG, "onProxyLoaded: $proxyPath")
        val proxyFile = File(proxyPath)
        val fileName = proxyFile.name // e.g. libandengine.so
        val containerDir = proxyFile.parentFile?.parentFile ?: pluginDir
        val lib32File = File(containerDir, "lib32/$fileName")

        if (!lib32File.exists()) {
            Log.e(TAG, "Guest library not found: ${lib32File.absolutePath}")
            return -1
        }

        if (!nativeLoadLibrary(lib32File.absolutePath)) {
            Log.e(TAG, "nativeLoadLibrary failed for ${lib32File.absolutePath}")
            return -1
        }

        // Bind statically exported Java_* symbols
        activeClassLoader?.let { cl ->
            bindExports(cl)
        }

        // Call guest JNI_OnLoad if present
        val jniVersion = nativeCallJniOnLoad()
        Log.i(TAG, "Loaded $fileName; JNI version = 0x${Integer.toHexString(jniVersion)}")
        return jniVersion
    }

    /**
     * Inspects native exports from the loaded guest library and registers matching methods into ART.
     */
    fun bindExports(classLoader: ClassLoader) {
        val exports = nativeGetExports()
        for (sym in exports) {
            if (!sym.startsWith("Java_")) continue

            val decoded = decodeJniExport(sym) ?: continue
            val className = decoded.className.replace('/', '.')
            val methodName = decoded.method

            try {
                val clazz = classLoader.loadClass(className)
                val matchingMethods = clazz.declaredMethods.filter { m ->
                    Modifier.isNative(m.modifiers) && m.name == methodName
                }

                for (method in matchingMethods) {
                    val sig = getMethodSignature(method)
                    // If export specifies argument types, check match
                    if (decoded.arguments != null && !sig.startsWith(decoded.arguments)) {
                        continue
                    }
                    val guestFn = nativeGetGuestSymbol(sym)
                    if (guestFn != 0L) {
                        nativeRegisterMethod(clazz, methodName, sig, guestFn)
                    }
                }
            } catch (e: ClassNotFoundException) {
                Log.w(TAG, "Class $className for export $sym not found; skipping")
            } catch (e: Throwable) {
                Log.w(TAG, "Failed to bind export $sym", e)
            }
        }
    }

    private data class DecodedExport(
        val className: String,
        val method: String,
        val arguments: String?
    )

    private fun decodeJniExport(symbol: String): DecodedExport? {
        val withoutPrefix = symbol.removePrefix("Java_")
        val parts = withoutPrefix.split("__")
        val fullPath = parts[0]
        val args = if (parts.size > 1) "(${parts[1]})" else null

        // Decode escapes: _1 -> _, _2 -> ;, _3 -> [, _0xxxx -> unicode, _ -> /
        val decoded = decodeMangledString(fullPath)
        val slashIndex = decoded.lastIndexOf('/')
        if (slashIndex <= 0 || slashIndex >= decoded.length - 1) return null

        val className = decoded.substring(0, slashIndex)
        val methodName = decoded.substring(slashIndex + 1)
        return DecodedExport(className, methodName, args)
    }

    private fun decodeMangledString(s: String): String {
        val sb = StringBuilder()
        var i = 0
        while (i < s.length) {
            if (s[i] == '_') {
                if (i + 1 < s.length) {
                    when (s[i + 1]) {
                        '1' -> { sb.append('_'); i += 2; continue }
                        '2' -> { sb.append(';'); i += 2; continue }
                        '3' -> { sb.append('['); i += 2; continue }
                        '0' -> {
                            if (i + 5 < s.length) {
                                val hex = s.substring(i + 2, i + 6)
                                try {
                                    sb.append(hex.toInt(16).toChar())
                                    i += 6
                                    continue
                                } catch (_: Exception) {}
                            }
                        }
                    }
                }
                sb.append('/')
                i++
            } else {
                sb.append(s[i])
                i++
            }
        }
        return sb.toString()
    }

    private fun getMethodSignature(method: Method): String {
        val sb = StringBuilder("(")
        for (param in method.parameterTypes) {
            sb.append(getTypeDescriptor(param))
        }
        sb.append(")")
        sb.append(getTypeDescriptor(method.returnType))
        return sb.toString()
    }

    private fun getTypeDescriptor(clazz: Class<*>): String {
        return when {
            clazz == java.lang.Boolean.TYPE -> "Z"
            clazz == java.lang.Byte.TYPE -> "B"
            clazz == java.lang.Character.TYPE -> "C"
            clazz == java.lang.Short.TYPE -> "S"
            clazz == java.lang.Integer.TYPE -> "I"
            clazz == java.lang.Long.TYPE -> "J"
            clazz == java.lang.Float.TYPE -> "F"
            clazz == java.lang.Double.TYPE -> "D"
            clazz == java.lang.Void.TYPE -> "V"
            clazz.isArray -> "[" + getTypeDescriptor(clazz.componentType ?: Any::class.java)
            else -> "L" + clazz.name.replace('.', '/') + ";"
        }
    }
}
