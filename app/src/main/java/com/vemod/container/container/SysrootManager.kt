package com.vemod.container.container

import android.content.Context
import android.util.Log
import com.vemod.container.bridge.VemodBridge
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.zip.GZIPInputStream

/**
 * Manages the 32-bit ARM sysroot runtime environment (AOSP Nougat 7.0 Bionic + custom libbionic_guest.so).
 *
 * Downloads and verifies sysroot packages on first boot to comply with Google SDK redistribution terms
 * and reduce the base APK installation footprint.
 */
object SysrootManager {
    private const val TAG = "SysrootManager"
    const val CURRENT_SYSROOT_VERSION = "v24-0.5.0-beta"
    const val SYSROOT_ARCHIVE_NAME = "sysroot-arm32-v24.tar.gz"
    const val DOWNLOAD_URL = "https://github.com/psychoStark/vemod/releases/download/0.5.0-beta/$SYSROOT_ARCHIVE_NAME"
    const val FALLBACK_DOWNLOAD_URL = "https://github.com/psychoStark/vemod/releases/download/v0.5.0-beta/$SYSROOT_ARCHIVE_NAME"

    fun getSysrootDir(context: Context): File {
        return File(context.filesDir, "sysroot")
    }

    fun isSysrootReady(context: Context): Boolean {
        val sysrootDir = getSysrootDir(context)
        val versionFile = File(sysrootDir, ".installed")
        val libc = File(sysrootDir, "libc.so")
        return sysrootDir.isDirectory && versionFile.exists() && versionFile.readText().trim() == CURRENT_SYSROOT_VERSION && libc.exists()
    }

    @Synchronized
    fun ensureSysroot(context: Context) {
        val sysrootDir = getSysrootDir(context)
        if (isSysrootReady(context)) {
            Log.i(TAG, "Sysroot already up-to-date at ${sysrootDir.absolutePath}")
            VemodBridge.nativeSetSysrootDir(sysrootDir.absolutePath)
            return
        }

        Log.i(TAG, "Initializing sysroot environment at ${sysrootDir.absolutePath}...")
        sysrootDir.mkdirs()

        // 1. Try unpacking from bundled asset archive if present
        var extracted = false
        val assetNames = listOf(SYSROOT_ARCHIVE_NAME, "sysroot/$SYSROOT_ARCHIVE_NAME")
        for (assetPath in assetNames) {
            try {
                context.assets.open(assetPath).use { input ->
                    Log.i(TAG, "Extracting bundled sysroot archive from assets: $assetPath")
                    extractTarGz(input, sysrootDir)
                    extracted = true
                }
                break
            } catch (_: Throwable) {}
        }

        // 2. If not bundled in assets, attempt download from GitHub Releases
        if (!extracted) {
            val tempArchive = File(context.cacheDir, SYSROOT_ARCHIVE_NAME)
            val urls = listOf(DOWNLOAD_URL, FALLBACK_DOWNLOAD_URL)
            for (url in urls) {
                try {
                    Log.i(TAG, "Downloading sysroot package from $url...")
                    downloadFile(url, tempArchive)
                    tempArchive.inputStream().use { input ->
                        extractTarGz(input, sysrootDir)
                    }
                    extracted = true
                    Log.i(TAG, "Sysroot downloaded and extracted successfully from $url")
                    break
                } catch (e: Throwable) {
                    Log.w(TAG, "Failed to download sysroot from $url", e)
                } finally {
                    if (tempArchive.exists()) tempArchive.delete()
                }
            }
        }

        // 3. Fallback: check if uncompressed sysroot files exist in assets/sysroot
        if (!extracted && !File(sysrootDir, "libc.so").exists()) {
            try {
                val files = context.assets.list("sysroot") ?: emptyArray()
                for (filename in files) {
                    if (filename.endsWith(".so") || filename.endsWith(".txt")) {
                        val outFile = File(sysrootDir, filename)
                        context.assets.open("sysroot/$filename").use { input ->
                            outFile.outputStream().use { output -> input.copyTo(output) }
                        }
                    }
                }
                if (File(sysrootDir, "libc.so").exists()) {
                    extracted = true
                    Log.i(TAG, "Copied sysroot files from assets fallback")
                }
            } catch (e: Throwable) {
                Log.w(TAG, "Asset fallback extraction failed: ${e.message}")
            }
        }

        if (File(sysrootDir, "libc.so").exists()) {
            File(sysrootDir, ".installed").writeText(CURRENT_SYSROOT_VERSION)
            Log.i(TAG, "Sysroot successfully initialized ($CURRENT_SYSROOT_VERSION)")
        } else {
            Log.e(TAG, "Sysroot initialization incomplete: libc.so not found")
        }

        VemodBridge.nativeSetSysrootDir(sysrootDir.absolutePath)
    }

    private fun downloadFile(urlStr: String, destination: File) {
        val oldPolicy = android.os.StrictMode.getThreadPolicy()
        android.os.StrictMode.setThreadPolicy(android.os.StrictMode.ThreadPolicy.Builder().permitAll().build())
        try {
            val url = URL(urlStr)
            val conn = url.openConnection() as HttpURLConnection
            conn.setRequestProperty("User-Agent", "Vemod-SysrootDownloader/0.5.0")
            conn.connectTimeout = 15000
            conn.readTimeout = 30000
            conn.instanceFollowRedirects = true

            val responseCode = conn.responseCode
            if (responseCode == HttpURLConnection.HTTP_MOVED_PERM || 
                responseCode == HttpURLConnection.HTTP_MOVED_TEMP || 
                responseCode == 307 || 
                responseCode == 308) {
                val newUrl = conn.getHeaderField("Location")
                if (!newUrl.isNullOrEmpty()) {
                    downloadFile(newUrl, destination)
                    return
                }
            }

            if (responseCode != HttpURLConnection.HTTP_OK) {
                throw java.io.IOException("HTTP $responseCode while downloading $urlStr")
            }

            conn.inputStream.use { input ->
                destination.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
        } finally {
            android.os.StrictMode.setThreadPolicy(oldPolicy)
        }
    }

    fun extractTarGz(inputStream: InputStream, targetDir: File) {
        targetDir.mkdirs()
        GZIPInputStream(inputStream.buffered()).use { gzis ->
            val header = ByteArray(512)
            while (readFully(gzis, header, 512)) {
                // Check for EOF (two consecutive 512-byte zero blocks, or first block all zeroes)
                if (header.all { it == 0.toByte() }) {
                    break
                }

                val name = String(header, 0, 100, Charsets.US_ASCII).trim('\u0000', ' ')
                if (name.isEmpty()) continue

                val sizeStr = String(header, 124, 12, Charsets.US_ASCII).trim('\u0000', ' ')
                val size = try {
                    if (sizeStr.isNotEmpty()) java.lang.Long.parseLong(sizeStr, 8) else 0L
                } catch (_: Throwable) { 0L }

                val typeFlag = header[156].toInt().toChar()
                val outFile = File(targetDir, name)

                // Protect against Zip Slip / path traversal
                if (!outFile.canonicalPath.startsWith(targetDir.canonicalPath)) {
                    throw SecurityException("Arbitrary file write path detected in tar: $name")
                }

                if (typeFlag == '5' || name.endsWith("/")) {
                    outFile.mkdirs()
                } else {
                    outFile.parentFile?.mkdirs()
                    var remaining = size
                    val buf = ByteArray(8192)
                    FileOutputStream(outFile).use { fos ->
                        while (remaining > 0) {
                            val toRead = Math.min(remaining, buf.size.toLong()).toInt()
                            val read = gzis.read(buf, 0, toRead)
                            if (read < 0) break
                            fos.write(buf, 0, read)
                            remaining -= read
                        }
                    }
                    outFile.setExecutable(true, false)
                }

                // Discard padding to 512-byte boundary
                val pad = (512 - (size % 512)) % 512
                if (pad > 0) {
                    skipFully(gzis, pad)
                }
            }
        }
    }

    private fun readFully(input: InputStream, buffer: ByteArray, length: Int): Boolean {
        var offset = 0
        while (offset < length) {
            val read = input.read(buffer, offset, length - offset)
            if (read < 0) return false
            offset += read
        }
        return true
    }

    private fun skipFully(input: InputStream, bytesToSkip: Long) {
        var remaining = bytesToSkip
        val buffer = ByteArray(512)
        while (remaining > 0) {
            val skipped = input.skip(remaining)
            if (skipped > 0) {
                remaining -= skipped
            } else {
                val toRead = Math.min(remaining, buffer.size.toLong()).toInt()
                val read = input.read(buffer, 0, toRead)
                if (read < 0) break
                remaining -= read
            }
        }
    }
}
