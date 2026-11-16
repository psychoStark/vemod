package com.vemod.container.container

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.net.Uri
import android.util.Log
import androidx.core.content.FileProvider
import com.android.apksig.ApkSigner
import com.vemod.container.model.ContainerApp
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.KeyStore
import java.security.PrivateKey
import java.security.cert.X509Certificate
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

object StubApkGenerator {
    private const val TAG = "StubApkGenerator"
    const val STUB_PKG_PREFIX = "com.vemod.container_"

    fun getStubPackageName(guestPackage: String): String {
        return "$STUB_PKG_PREFIX$guestPackage"
    }

    fun isStubInstalled(context: Context, guestPackage: String): Boolean {
        val stubPkg = getStubPackageName(guestPackage)
        return try {
            context.packageManager.getPackageInfo(stubPkg, 0)
            true
        } catch (_: Throwable) {
            // Also check old stub package prefix for backward compatibility
            try {
                context.packageManager.getPackageInfo("com.vemod.stub.$guestPackage", 0)
                true
            } catch (_: Throwable) {
                false
            }
        }
    }

    fun getStubApkFile(app: ContainerApp): File {
        return File(app.containerDir, "${app.packageName}.stub.apk")
    }

    fun generateStubApk(context: Context, app: ContainerApp): File {
        val targetApk = getStubApkFile(app)
        val tempUnsigned = File(app.containerDir, "temp_unsigned.apk")

        try {
            val stubPkg = getStubPackageName(app.packageName)

            // 1. Load and modify template AndroidManifest.xml
            val rawManifest = context.assets.open("stub/template_manifest.axml").use { it.readBytes() }
            val manifestReplacements = mapOf(
                "com.vemod.stub.template" to stubPkg,
                "Stub App" to app.appName
            )
            val modifiedManifest = replaceAxmlStrings(rawManifest, manifestReplacements)

            // 2. Load and modify template resources.arsc
            val rawArsc = context.assets.open("stub/template_resources.arsc").use { it.readBytes() }
            val modifiedArsc = replaceResourcesArscPackage(rawArsc, stubPkg)

            // 3. Load classes.dex
            val dexBytes = context.assets.open("stub/template_classes.dex").use { it.readBytes() }

            // 4. Load app icon (PNG)
            val iconBytes = if (app.iconFile != null && app.iconFile.exists()) {
                app.iconFile.readBytes()
            } else {
                ByteArray(0)
            }

            // 5. Build unsigned zip
            if (tempUnsigned.exists()) tempUnsigned.delete()
            FileOutputStream(tempUnsigned).use { fos ->
                AlignedZipOutputStream(fos).use { zos ->
                    // AndroidManifest.xml (compressed)
                    zos.putDeflatedEntry("AndroidManifest.xml", modifiedManifest)

                    // resources.arsc (MUST be STORED and 4-byte aligned)
                    zos.putStoredAlignedEntry("resources.arsc", modifiedArsc, 4)

                    // classes.dex (compressed)
                    zos.putDeflatedEntry("classes.dex", dexBytes)

                    // App icon
                    if (iconBytes.isNotEmpty()) {
                        zos.putDeflatedEntry("res/drawable/ic_launcher.png", iconBytes)
                    }
                }
            }

            // 6. Sign APK with v1, v2, v3 via official ApkSigner
            val ks = KeyStore.getInstance("PKCS12")
            context.assets.open("stub/debug.p12").use { ks.load(it, "android".toCharArray()) }
            val privateKey = ks.getKey("androiddebugkey", "android".toCharArray()) as PrivateKey
            val cert = ks.getCertificate("androiddebugkey") as X509Certificate

            val signerConfig = ApkSigner.SignerConfig.Builder(
                "androiddebugkey",
                privateKey,
                listOf(cert)
            ).build()

            if (targetApk.exists()) targetApk.delete()

            val apkSigner = ApkSigner.Builder(listOf(signerConfig))
                .setInputApk(tempUnsigned)
                .setOutputApk(targetApk)
                .setV1SigningEnabled(true)
                .setV2SigningEnabled(true)
                .setV3SigningEnabled(true)
                .build()

            apkSigner.sign()
            Log.i(TAG, "Generated and signed stub APK: ${targetApk.absolutePath} (${targetApk.length()} bytes)")
            return targetApk
        } finally {
            if (tempUnsigned.exists()) {
                tempUnsigned.delete()
            }
        }
    }

    fun installStub(context: Context, app: ContainerApp) {
        val apkFile = generateStubApk(context, app)

        try {
            val pm = context.packageManager
            val installer = pm.packageInstaller
            val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
            params.setAppPackageName(getStubPackageName(app.packageName))

            val sessionId = installer.createSession(params)
            val session = installer.openSession(sessionId)

            apkFile.inputStream().use { input ->
                session.openWrite("package", 0, apkFile.length()).use { output ->
                    input.copyTo(output)
                    session.fsync(output)
                }
            }

            val intent = Intent(context, StubInstallReceiver::class.java).apply {
                action = "com.vemod.action.STUB_INSTALLED"
                putExtra("package_name", app.packageName)
            }
            val pendingIntent = PendingIntent.getBroadcast(
                context,
                sessionId,
                intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE
            )

            session.commit(pendingIntent.intentSender)
            Log.i(TAG, "Committed PackageInstaller session $sessionId for ${app.packageName}")
        } catch (e: Throwable) {
            Log.w(TAG, "PackageInstaller session failed, falling back to ACTION_VIEW", e)
            val uri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", apkFile)
            val intent = Intent(Intent.ACTION_VIEW).apply {
                setDataAndType(uri, "application/vnd.android.package-archive")
                flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_GRANT_READ_URI_PERMISSION
            }
            context.startActivity(intent)
        }
    }

    fun uninstallStub(context: Context, guestPackage: String) {
        val stubPkg = getStubPackageName(guestPackage)
        try {
            val intent = Intent(Intent.ACTION_UNINSTALL_PACKAGE).apply {
                data = Uri.parse("package:$stubPkg")
                flags = Intent.FLAG_ACTIVITY_NEW_TASK
            }
            context.startActivity(intent)
        } catch (e: Throwable) {
            Log.e(TAG, "Failed to launch uninstaller for $stubPkg", e)
        }
    }

    private fun replaceAxmlStrings(data: ByteArray, replacements: Map<String, String>): ByteArray {
        val buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        val mType = buf.short.toInt() and 0xFFFF
        val mHsize = buf.short.toInt() and 0xFFFF

        val spOffset = 8
        buf.position(spOffset)
        val spType = buf.short.toInt() and 0xFFFF
        val spHsize = buf.short.toInt() and 0xFFFF
        val spSize = buf.int
        val strCount = buf.int
        val styleCount = buf.int
        val flags = buf.int
        val strStart = buf.int
        val styleStart = buf.int

        val isUtf8 = (flags and (1 shl 8)) != 0

        val offsets = IntArray(strCount)
        for (i in 0 until strCount) {
            offsets[i] = buf.int
        }

        val strings = ArrayList<String>(strCount)
        for (i in 0 until strCount) {
            val pos = spOffset + strStart + offsets[i]
            buf.position(pos)
            if (isUtf8) {
                buf.get() // char len
                val byteLen = buf.get().toInt() and 0xFF
                val bytes = ByteArray(byteLen)
                buf.get(bytes)
                strings.add(String(bytes, Charsets.UTF_8))
            } else {
                val u16Len = buf.short.toInt() and 0xFFFF
                val chars = CharArray(u16Len)
                for (j in 0 until u16Len) {
                    chars[j] = buf.char
                }
                strings.add(String(chars))
            }
        }

        val newStrings = strings.map { replacements[it] ?: it }

        val newStrData = ByteArrayOutputStream()
        val newOffsets = IntArray(strCount)

        for (i in 0 until strCount) {
            newOffsets[i] = newStrData.size()
            val s = newStrings[i]
            if (isUtf8) {
                val utf8Bytes = s.toByteArray(Charsets.UTF_8)
                newStrData.write(s.length and 0xFF)
                newStrData.write(utf8Bytes.size and 0xFF)
                newStrData.write(utf8Bytes)
                newStrData.write(0)
            } else {
                val u16Len = s.length
                newStrData.write(u16Len and 0xFF)
                newStrData.write((u16Len shr 8) and 0xFF)
                val u16Bytes = s.toByteArray(Charsets.UTF_16LE)
                newStrData.write(u16Bytes)
                newStrData.write(0)
                newStrData.write(0)
            }
        }

        while (newStrData.size() % 4 != 0) {
            newStrData.write(0)
        }

        val newStrStart = 28 + strCount * 4
        val newSpSize = newStrStart + newStrData.size()

        val out = ByteArrayOutputStream()
        val outBuf = ByteBuffer.allocate(spOffset + newStrStart).order(ByteOrder.LITTLE_ENDIAN)

        outBuf.putShort(mType.toShort())
        outBuf.putShort(mHsize.toShort())
        outBuf.putInt(0) // placeholder for total size

        outBuf.putShort(spType.toShort())
        outBuf.putShort(spHsize.toShort())
        outBuf.putInt(newSpSize)
        outBuf.putInt(strCount)
        outBuf.putInt(styleCount)
        outBuf.putInt(flags)
        outBuf.putInt(newStrStart)
        outBuf.putInt(styleStart)

        for (off in newOffsets) {
            outBuf.putInt(off)
        }

        out.write(outBuf.array())
        out.write(newStrData.toByteArray())

        val restPos = spOffset + spSize
        val restLen = data.size - restPos
        out.write(data, restPos, restLen)

        val finalBytes = out.toByteArray()
        val finalBuf = ByteBuffer.wrap(finalBytes).order(ByteOrder.LITTLE_ENDIAN)
        finalBuf.putInt(4, finalBytes.size)
        return finalBytes
    }

    private fun replaceResourcesArscPackage(data: ByteArray, newPackage: String): ByteArray {
        val buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        buf.position(16)
        val spSize = buf.int
        val pkgChunkPos = 12 + spSize
        val pkgNameOffset = pkgChunkPos + 12

        val result = data.copyOf()
        for (i in 0 until 256) {
            result[pkgNameOffset + i] = 0
        }
        val u16Bytes = newPackage.toByteArray(Charsets.UTF_16LE)
        val copyLen = minOf(u16Bytes.size, 256)
        System.arraycopy(u16Bytes, 0, result, pkgNameOffset, copyLen)
        return result
    }

    /**
     * Minimal custom ZipOutputStream helper that aligns specific STORED entries to 4-byte boundaries.
     */
    private class AlignedZipOutputStream(out: FileOutputStream) : ZipOutputStream(out) {
        private var bytesWritten = 0L

        fun putDeflatedEntry(name: String, data: ByteArray) {
            val entry = ZipEntry(name)
            entry.method = ZipEntry.DEFLATED
            putNextEntry(entry)
            write(data)
            closeEntry()
        }

        fun putStoredAlignedEntry(name: String, data: ByteArray, alignment: Int = 4) {
            val entry = ZipEntry(name)
            entry.method = ZipEntry.STORED
            entry.size = data.size.toLong()
            entry.compressedSize = data.size.toLong()

            val crc = CRC32()
            crc.update(data)
            entry.crc = crc.value

            // Compute local header size: 30 + nameLen
            val nameBytes = name.toByteArray(Charsets.UTF_8)
            val headerBase = 30 + nameBytes.size
            val currentOffset = bytesWritten
            val dataOffset = currentOffset + headerBase

            val mod = (dataOffset % alignment).toInt()
            if (mod != 0) {
                val pad = alignment - mod
                entry.extra = ByteArray(pad)
            }

            putNextEntry(entry)
            write(data)
            closeEntry()
        }

        override fun write(b: ByteArray, off: Int, len: Int) {
            super.write(b, off, len)
            bytesWritten += len
        }

        override fun write(b: Int) {
            super.write(b)
            bytesWritten++
        }
    }
}
