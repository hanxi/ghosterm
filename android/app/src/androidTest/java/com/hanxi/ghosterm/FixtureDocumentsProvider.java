package com.hanxi.ghosterm;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/** Test-APK-only provider. Java keeps its standalone process independent of target-app Kotlin classes. */
public final class FixtureDocumentsProvider extends DocumentsProvider {
    private final Map<String, Integer> writes = new ConcurrentHashMap<>();
    private final Map<String, String> documents = new LinkedHashMap<>();
    private File directory;
    @Override public boolean onCreate() {
        directory = new File(getContext().getFilesDir(), "saf-fixtures");
        if (!directory.isDirectory() && !directory.mkdirs()) return false;
        for (String id : new String[] {"valid", "oversize", "encoding", "private", "include", "denied", "large", "empty", "nul"}) {
            documents.put(id, id + ".conf");
            byte[] data;
            switch (id) {
                case "valid": data = utf8("# imported 🔑\r\nHost imported-host\r\n  HostName imported.test\r\n  User owner\r\n"); break;
                case "oversize": data = new byte[1024 * 1024 + 1]; Arrays.fill(data, (byte)'#'); break;
                case "encoding": data = new byte[] {(byte)0xc3, 0x28}; break;
                case "private": data = utf8("-----BEGIN OPENSSH PRIVATE KEY-----\nnot-a-key\n-----END OPENSSH PRIVATE KEY-----\n"); break;
                case "include": data = utf8("Include /nonexistent-ghosterm-test/config\n"); break;
                case "large": data = utf8("#" + "a".repeat(8190) + "🔑\nHost after-preview\n"); break;
                case "empty": data = new byte[0]; break;
                case "nul": data = utf8("Host before\0Host hidden\n"); break;
                default: data = utf8("Host denied\n");
            }
            try (FileOutputStream output = new FileOutputStream(new File(directory, id))) { output.write(data); }
            catch (IOException failure) { throw new IllegalStateException("Cannot create test document", failure); }
        }
        return true;
    }
    private static byte[] utf8(String text) { return text.getBytes(StandardCharsets.UTF_8); }
    private static void row(MatrixCursor cursor, Map<String, Object> values) {
        Object[] cells = new Object[cursor.getColumnCount()];
        for (int i = 0; i < cells.length; i++) cells[i] = values.get(cursor.getColumnNames()[i]);
        cursor.addRow(cells);
    }
    @Override public Cursor queryRoots(String[] projection) {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : new String[] {
            Root.COLUMN_ROOT_ID, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_TITLE, Root.COLUMN_FLAGS, Root.COLUMN_MIME_TYPES});
        Map<String, Object> values = new HashMap<>();
        values.put(Root.COLUMN_ROOT_ID, "fixture-root"); values.put(Root.COLUMN_DOCUMENT_ID, "root");
        values.put(Root.COLUMN_TITLE, "Ghosterm Test Files"); values.put(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_IS_CHILD);
        values.put(Root.COLUMN_MIME_TYPES, "text/plain"); row(cursor, values); return cursor;
    }
    private MatrixCursor cursor(String[] projection) {
        return new MatrixCursor(projection != null ? projection : new String[] {Document.COLUMN_DOCUMENT_ID,
            Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_FLAGS,
            Document.COLUMN_SIZE, Document.COLUMN_SUMMARY});
    }
    private void document(MatrixCursor cursor, String id) throws FileNotFoundException {
        boolean root = "root".equals(id);
        if (!root && !documents.containsKey(id)) throw new FileNotFoundException(id);
        Map<String, Object> values = new HashMap<>();
        values.put(Document.COLUMN_DOCUMENT_ID, id);
        values.put(Document.COLUMN_DISPLAY_NAME, root ? "Ghosterm Test Files" : documents.get(id));
        values.put(Document.COLUMN_MIME_TYPE, root ? Document.MIME_TYPE_DIR : "text/plain");
        values.put(Document.COLUMN_FLAGS, root ? 0 : Document.FLAG_SUPPORTS_WRITE);
        values.put(Document.COLUMN_SIZE, root ? 0L : new File(directory, id).length());
        values.put(Document.COLUMN_SUMMARY, "writes=" + writes.getOrDefault(id, 0)); row(cursor, values);
    }
    @Override public Cursor queryDocument(String id, String[] projection) throws FileNotFoundException {
        MatrixCursor cursor = cursor(projection); document(cursor, id); return cursor;
    }
    @Override public Cursor queryChildDocuments(String parent, String[] projection, String sort) throws FileNotFoundException {
        if (!"root".equals(parent)) throw new FileNotFoundException(parent);
        MatrixCursor cursor = cursor(projection);
        for (String id : documents.keySet()) document(cursor, id);
        return cursor;
    }
    @Override public boolean isChildDocument(String parent, String id) {
        return "root".equals(parent) && documents.containsKey(id);
    }
    @Override public ParcelFileDescriptor openDocument(String id, String mode, CancellationSignal signal) throws FileNotFoundException {
        if (signal != null) signal.throwIfCanceled();
        if (!documents.containsKey(id)) throw new FileNotFoundException(id);
        if ("denied".equals(id)) throw new SecurityException("Fixture provider denied read");
        if (mode.contains("w")) writes.merge(id, 1, Integer::sum);
        return ParcelFileDescriptor.open(new File(directory, id), ParcelFileDescriptor.parseMode(mode));
    }
}
