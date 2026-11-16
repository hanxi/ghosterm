package com.hanxi.ghosterm;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileNotFoundException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;

/** Public, disposable bytes only. Never included in the production APK. */
public final class PluginSignatureStreamProvider extends ContentProvider {
    static final Uri URI = Uri.parse("content://com.hanxi.ghosterm.test.signature-stream/material");
    private volatile String mode = "regular";
    private volatile CountDownLatch blocked = new CountDownLatch(1);
    private volatile boolean opening;
    private final AtomicInteger cancelled = new AtomicInteger();
    private final java.util.List<ParcelFileDescriptor> writers = new java.util.ArrayList<>();
    private final AtomicInteger opened = new AtomicInteger();
    private ParcelFileDescriptor shared;
    private File file() { return new File(getContext().getCacheDir(), "signature-stream-public"); }
    @Override public boolean onCreate() { return true; }
    @Override public synchronized Bundle call(String method, String arg, Bundle extras) {
        Bundle result = new Bundle();
        try {
            if ("set".equals(method)) {
                blocked.countDown(); blocked = new CountDownLatch(1); opening = false; cancelled.set(0); opened.set(0);
                for (ParcelFileDescriptor writer : writers) writer.close(); writers.clear();
                if (shared != null) { shared.close(); shared = null; }
                mode = extras.getString("mode", "regular");
                byte[] bytes = extras.getByteArray("bytes");
                if (bytes == null || bytes.length > 200000) throw new IllegalArgumentException();
                try (java.io.FileOutputStream out = new java.io.FileOutputStream(file())) { out.write(bytes); }
                if (extras.containsKey("size")) {
                    long size = extras.getLong("size");
                    if (size < 0 || size > 75497473L) throw new IllegalArgumentException();
                    try (java.io.RandomAccessFile out = new java.io.RandomAccessFile(file(), "rw")) { out.setLength(size); }
                }
                if ("shared".equals(mode)) shared = ParcelFileDescriptor.open(file(), ParcelFileDescriptor.MODE_READ_ONLY);
            } else if ("complete".equals(method)) {
                if (!"gated-pipe".equals(mode) || writers.isEmpty()) throw new IllegalStateException();
                byte[] bytes = java.nio.file.Files.readAllBytes(file().toPath());
                if (bytes.length > 4096) throw new IllegalArgumentException();
                for (ParcelFileDescriptor writer : writers) {
                    if (android.system.Os.write(writer.getFileDescriptor(), bytes, 0, bytes.length) != bytes.length)
                        throw new java.io.IOException("short fixture write");
                    writer.close();
                }
                writers.clear();
            } else if ("release".equals(method)) {
                blocked.countDown();
                for (ParcelFileDescriptor writer : writers) writer.close(); writers.clear();
                if (shared != null) { shared.close(); shared = null; }
                if (!file().delete() && file().exists()) throw new java.io.IOException("cleanup");
            } else if (!"state".equals(method)) throw new IllegalArgumentException();
            result.putBoolean("ok", true); result.putBoolean("opening", opening); result.putInt("cancelled", cancelled.get()); result.putInt("opened", opened.get());
            if (shared != null) for (String line : java.nio.file.Files.readAllLines(java.nio.file.Paths.get("/proc/self/fdinfo/" + shared.getFd()))) {
                if (line.startsWith("flags:")) result.putLong("flags", Long.parseLong(line.substring(6).trim(), 8));
            }
        } catch (Exception failure) { result.putString("error", failure.getClass().getSimpleName()); }
        return result;
    }
    @Override public ParcelFileDescriptor openFile(Uri uri, String access, CancellationSignal signal) throws FileNotFoundException {
        if (!"r".equals(access)) throw new FileNotFoundException("read only");
        opened.incrementAndGet();
        if ("shared".equals(mode)) {
            try { return ParcelFileDescriptor.dup(shared.getFileDescriptor()); }
            catch (java.io.IOException failure) { throw new FileNotFoundException("shared"); }
        }
        if ("blocked-open".equals(mode)) {
            CountDownLatch waiting = blocked;
            if (signal != null) signal.setOnCancelListener(() -> { cancelled.incrementAndGet(); waiting.countDown(); });
            opening = true;
            try { waiting.await(); } catch (InterruptedException failure) { Thread.currentThread().interrupt(); }
            if (signal != null) signal.throwIfCanceled();
            throw new FileNotFoundException("released probe");
        }
        if ("error-pipe".equals(mode)) {
            try {
                ParcelFileDescriptor[] pair = ParcelFileDescriptor.createReliablePipe();
                byte[] bytes = java.nio.file.Files.readAllBytes(file().toPath());
                android.system.Os.write(pair[1].getFileDescriptor(), bytes, 0, bytes.length);
                pair[1].closeWithError("synthetic public stream failure");
                return pair[0];
            } catch (Exception failure) { throw new FileNotFoundException("reliable pipe"); }
        }
        if ("stall".equals(mode) || "gated-pipe".equals(mode)) {
            try {
                ParcelFileDescriptor[] pair = "gated-pipe".equals(mode) ? ParcelFileDescriptor.createReliablePipe() : ParcelFileDescriptor.createPipe();
                synchronized (this) { writers.add(pair[1]); }
                opening = true; return pair[0];
            } catch (java.io.IOException failure) { throw new FileNotFoundException("pipe"); }
        }
        return ParcelFileDescriptor.open(file(), "read-write".equals(mode) ? ParcelFileDescriptor.MODE_READ_WRITE : ParcelFileDescriptor.MODE_READ_ONLY);
    }
    @Override public android.content.res.AssetFileDescriptor openAssetFile(Uri uri, String mode, CancellationSignal signal) throws FileNotFoundException {
        return new android.content.res.AssetFileDescriptor(openFile(uri, mode, signal), 0, android.content.res.AssetFileDescriptor.UNKNOWN_LENGTH);
    }
    @Override public android.content.res.AssetFileDescriptor openTypedAssetFile(Uri uri, String mime, Bundle options, CancellationSignal signal) throws FileNotFoundException {
        return openAssetFile(uri, "r", signal);
    }
    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException { return openFile(uri, mode, null); }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        MatrixCursor c = new MatrixCursor(new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE });
        c.addRow(new Object[] { "misleading.sig", 0L }); return c;
    }
    @Override public String getType(Uri uri) { return "application/octet-stream"; }
    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String selection, String[] args) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }
}
