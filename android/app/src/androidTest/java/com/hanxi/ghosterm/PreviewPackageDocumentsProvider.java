package com.hanxi.ghosterm;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.content.Context;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsProvider;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.util.Base64;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

/** Test-only SAF tree serving a fixed package snapshot from the test APK's private files. */
public final class PreviewPackageDocumentsProvider extends DocumentsProvider {
    static final String AUTHORITY = "com.hanxi.ghosterm.test.preview-package";

    static File packageRoot(Context context) {
        return new File(context.getFilesDir(), "preview-package-tree");
    }

    private File packageRoot() {
        return packageRoot(getContext());
    }

    private static void clearChildren(File root) throws java.io.IOException {
        File[] children = root.listFiles();
        if (children == null) throw new java.io.IOException("cannot list fixture root");
        for (File child : children) {
            if (child.isDirectory() && !Files.isSymbolicLink(child.toPath())) clearChildren(child);
            if (!child.delete() && child.exists()) {
                throw new java.io.IOException("cannot remove fixture member: " + child.getName());
            }
        }
    }

    private File resolve(String id) throws FileNotFoundException {
        File root = packageRoot();
        File file = "preview-root".equals(id) ? root : new File(root, id);
        try {
            String base = root.getCanonicalPath();
            String candidate = file.getCanonicalPath();
            if (!candidate.equals(base) && !candidate.startsWith(base + File.separator)) {
                throw new FileNotFoundException(id);
            }
        } catch (java.io.IOException failure) {
            throw new FileNotFoundException(id);
        }
        if (!file.exists() || Files.isSymbolicLink(file.toPath())) throw new FileNotFoundException(id);
        return file;
    }

    @Override public boolean onCreate() {
        File root = packageRoot();
        return root.isDirectory() || root.mkdirs();
    }

    static Bundle fixtureCommand(Context context, String method, Bundle extras) {
        Bundle result = new Bundle();
        try {
            if ("install".equals(method)) {
                File root = packageRoot(context);
                if (!root.isDirectory() && !root.mkdirs()) throw new java.io.IOException("cannot create fixture root");
                clearChildren(root);
                org.json.JSONObject files = new org.json.JSONObject(extras.getString("files"));
                java.util.Iterator<String> names = files.keys();
                while (names.hasNext()) {
                    String name = names.next();
                    File target = new File(root, name);
                    String base = root.getCanonicalPath();
                    if (!target.getCanonicalPath().startsWith(base + File.separator)) {
                        throw new java.io.IOException("fixture path escapes root");
                    }
                    File parent = target.getParentFile();
                    if (!parent.isDirectory() && !parent.mkdirs()) throw new java.io.IOException("cannot create fixture directory");
                    try (FileOutputStream output = new FileOutputStream(target)) {
                        output.write(Base64.decode(files.getString(name), Base64.DEFAULT));
                    }
                }
            } else if ("grant".equals(method)) {
                Uri tree = Uri.parse(extras.getString("uri"));
                context.grantUriPermission(extras.getString("target"), tree,
                    extras.getInt("flags", 0));
            } else if ("cleanup".equals(method)) {
                Uri tree = Uri.parse(extras.getString("uri"));
                context.revokeUriPermission(tree, extras.getInt("flags", 0));
                File root = packageRoot(context);
                clearChildren(root);
            } else {
                throw new IllegalArgumentException("unknown fixture command");
            }
            result.putBoolean("ok", true);
        } catch (Exception failure) {
            result.putBoolean("ok", false);
            result.putString("error", failure.getMessage());
        }
        return result;
    }

    private static void add(MatrixCursor cursor, Map<String, Object> values) {
        Object[] row = new Object[cursor.getColumnCount()];
        for (int i = 0; i < row.length; i++) row[i] = values.get(cursor.getColumnNames()[i]);
        cursor.addRow(row);
    }

    private MatrixCursor documentCursor(String[] projection) {
        return new MatrixCursor(projection != null ? projection : new String[] {
            Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
            Document.COLUMN_FLAGS, Document.COLUMN_SIZE
        });
    }

    @Override public Cursor queryRoots(String[] projection) {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : new String[] {
            Root.COLUMN_ROOT_ID, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_TITLE, Root.COLUMN_FLAGS,
            Root.COLUMN_MIME_TYPES
        });
        Map<String, Object> row = new HashMap<>();
        row.put(Root.COLUMN_ROOT_ID, "preview-package");
        row.put(Root.COLUMN_DOCUMENT_ID, "preview-root");
        row.put(Root.COLUMN_TITLE, "Ghosterm Official Preview Package Fixture");
        row.put(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_IS_CHILD);
        row.put(Root.COLUMN_MIME_TYPES, "*/*");
        add(cursor, row);
        return cursor;
    }

    private void addDocument(MatrixCursor cursor, File file, String id) {
        Map<String, Object> row = new HashMap<>();
        row.put(Document.COLUMN_DOCUMENT_ID, id);
        row.put(Document.COLUMN_DISPLAY_NAME, file.getName().isEmpty() ? "Preview Package" : file.getName());
        row.put(Document.COLUMN_MIME_TYPE, file.isDirectory() ? Document.MIME_TYPE_DIR : "application/octet-stream");
        row.put(Document.COLUMN_FLAGS, 0);
        row.put(Document.COLUMN_SIZE, file.isDirectory() ? 0L : file.length());
        add(cursor, row);
    }

    @Override public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        File file = resolve(documentId);
        MatrixCursor cursor = documentCursor(projection);
        addDocument(cursor, file, documentId);
        return cursor;
    }

    @Override public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder)
            throws FileNotFoundException {
        File parent = resolve(parentDocumentId);
        if (!parent.isDirectory()) throw new FileNotFoundException(parentDocumentId);
        MatrixCursor cursor = documentCursor(projection);
        File[] children = parent.listFiles();
        if (children == null) throw new FileNotFoundException(parentDocumentId);
        Arrays.sort(children, (a, b) -> a.getName().compareTo(b.getName()));
        String prefix = "preview-root".equals(parentDocumentId) ? "" : parentDocumentId + "/";
        for (File child : children) {
            String childId = prefix + child.getName();
            resolve(childId);
            addDocument(cursor, child, childId);
        }
        return cursor;
    }

    @Override public boolean isChildDocument(String parentDocumentId, String documentId) {
        try {
            File parent = resolve(parentDocumentId);
            File child = resolve(documentId);
            return parent.isDirectory() && !parent.equals(child) && child.getCanonicalPath().startsWith(
                parent.getCanonicalPath() + File.separator);
        } catch (Exception ignored) {
            return false;
        }
    }

    @Override public ParcelFileDescriptor openDocument(String documentId, String mode,
            CancellationSignal signal) throws FileNotFoundException {
        if (signal != null) signal.throwIfCanceled();
        File file = resolve(documentId);
        if (file.isDirectory() || mode.contains("w")) throw new FileNotFoundException(documentId);
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }
}
