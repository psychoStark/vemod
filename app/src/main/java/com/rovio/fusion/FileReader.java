package com.rovio.fusion;

import android.app.Activity;
import android.content.res.AssetManager;
import android.util.Log;
import com.vemod.container.proxy.VemodInstrumentation;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class FileReader {
    private static final String TAG = "FileReader";

    private static String normalizePath(String path) {
        if (path == null) return "";
        while (path.startsWith("/")) {
            path = path.substring(1);
        }
        if (path.startsWith("assets/")) {
            path = path.substring(7);
        }
        return path;
    }

    private static AssetManager getAssets() {
        try {
            Class<?> globalsClass = Class.forName("com.rovio.fusion.Globals", true, Thread.currentThread().getContextClassLoader());
            Method m = globalsClass.getMethod("getActivity");
            Activity act = (Activity) m.invoke(null);
            if (act != null && act.getAssets() != null) {
                return act.getAssets();
            }
        } catch (Throwable ignored) {}

        if (VemodInstrumentation.Companion.getGuestAssets() != null) {
            return VemodInstrumentation.Companion.getGuestAssets();
        }
        return null;
    }

    private static InputStream openAsset(String path) throws IOException {
        String norm = normalizePath(path);
        AssetManager am = getAssets();
        if (am != null) {
            try {
                return am.open(norm);
            } catch (IOException ignored) {}
        }
        File f = new File(norm);
        if (f.exists() && f.isFile()) {
            return new FileInputStream(f);
        }
        throw new IOException("Asset not found: " + path);
    }

    public static String[] enumerate(String path) {
        String norm = normalizePath(path);
        try {
            AssetManager am = getAssets();
            if (am != null) {
                String[] list = am.list(norm);
                if (list != null) return list;
            }
        } catch (IOException e) {
            Log.w(TAG, "enumerate failed for: " + path, e);
        }
        return new String[0];
    }

    public static boolean exists(String path) {
        return isFile(path) || isDirectory(path);
    }

    public static boolean isFile(String path) {
        try (InputStream is = openAsset(path)) {
            return true;
        } catch (Throwable e) {
            return false;
        }
    }

    public static boolean isDirectory(String path) {
        String norm = normalizePath(path);
        AssetManager am = getAssets();
        if (am == null) return false;
        try {
            String[] list = am.list(norm);
            return list != null && list.length > 0;
        } catch (Throwable e) {
            return false;
        }
    }

    public static ByteBuffer readFile(String path) {
        try {
            return readFileCompressed(path + ".zip");
        } catch (Throwable t) {
            return readFileUncompressed(path);
        }
    }

    public static ByteBuffer readFileCompressed(String path) throws IOException {
        try (InputStream is = openAsset(path);
             ZipInputStream zis = new ZipInputStream(is)) {
            ZipEntry ze = zis.getNextEntry();
            if (ze == null) throw new IOException("No entry in zip: " + path);
            ByteArrayOutputStream baos = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            int read;
            while ((read = zis.read(buf)) != -1) {
                baos.write(buf, 0, read);
            }
            byte[] data = baos.toByteArray();
            ByteBuffer bb = ByteBuffer.allocateDirect(data.length);
            bb.put(data);
            bb.rewind();
            Log.i(TAG, "readFileCompressed: loaded " + path + " (" + data.length + " bytes)");
            return bb;
        }
    }

    public static ByteBuffer readFileUncompressed(String path) {
        try (InputStream is = openAsset(path)) {
            ByteArrayOutputStream baos = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            int read;
            while ((read = is.read(buf)) != -1) {
                baos.write(buf, 0, read);
            }
            byte[] data = baos.toByteArray();
            ByteBuffer bb = ByteBuffer.allocateDirect(data.length);
            bb.put(data);
            bb.rewind();
            Log.i(TAG, "readFileUncompressed: loaded " + path + " (" + data.length + " bytes)");
            return bb;
        } catch (Throwable t) {
            Log.w(TAG, "readFileUncompressed failed for " + path + ": " + t.getMessage());
            return null;
        }
    }
}
