package com.hanxi.ghosterm;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/** Read-only tree in the test APK. IDs deliberately differ from display names and filesystem paths. */
public final class FilesDocumentsProvider extends DocumentsProvider {
    private File directory;
    private volatile String listingMode="ready";
    private final java.util.concurrent.atomic.AtomicInteger pendingQueries=new java.util.concurrent.atomic.AtomicInteger();
    private final java.util.concurrent.atomic.AtomicInteger pendingClosed=new java.util.concurrent.atomic.AtomicInteger();
    @Override public Bundle call(String method,String arg,Bundle extras) {
        Bundle result=new Bundle();
        if(method.equals("listing-mode")) { listingMode=arg;pendingQueries.set(0);pendingClosed.set(0); }
        else if(method.equals("listing-ready")) {
            listingMode="ready";
            getContext().getContentResolver().notifyChange(DocumentsContract.buildChildDocumentsUri(
                "com.hanxi.ghosterm.test.browser","opaque:folder"),null);
        } else if(!method.equals("listing-counts")) return super.call(method,arg,extras);
        result.putInt("queries",pendingQueries.get());result.putInt("closed",pendingClosed.get());return result;
    }
    private final java.util.concurrent.atomic.AtomicInteger slowStreams=new java.util.concurrent.atomic.AtomicInteger();
    private final java.util.concurrent.atomic.AtomicInteger slowNaturalEnds=new java.util.concurrent.atomic.AtomicInteger();
    @Override public boolean onCreate() {
        directory=new File(getContext().getFilesDir(),"browser-fixture");
        return directory.isDirectory() || directory.mkdirs();
    }
    private static void row(MatrixCursor cursor,Map<String,Object> values) {
        Object[] cells=new Object[cursor.getColumnCount()];
        for(int i=0;i<cells.length;i++) cells[i]=values.get(cursor.getColumnNames()[i]);
        cursor.addRow(cells);
    }
    @Override public Cursor queryRoots(String[] projection) {
        MatrixCursor cursor=new MatrixCursor(projection!=null?projection:new String[]{Root.COLUMN_ROOT_ID,Root.COLUMN_DOCUMENT_ID,Root.COLUMN_TITLE,Root.COLUMN_FLAGS,Root.COLUMN_MIME_TYPES});
        Map<String,Object> values=new HashMap<>();values.put(Root.COLUMN_ROOT_ID,"browser-root");values.put(Root.COLUMN_DOCUMENT_ID,"tree:root");
        values.put(Root.COLUMN_TITLE,"Ghosterm Browser Files");values.put(Root.COLUMN_FLAGS,Root.FLAG_SUPPORTS_IS_CHILD);values.put(Root.COLUMN_MIME_TYPES,"*/*");row(cursor,values);return cursor;
    }
    private String name(String id) throws FileNotFoundException {
        switch(id) {
            case "tree:root":return "Ghosterm Browser Files";
            case "opaque:folder":return "Folder";
            case "opaque:empty":return "Empty";
            case "opaque:denied":return "Denied";
            case "opaque:note":return "notes.txt";
            case "opaque:child":return "你好.txt";
            case "opaque:large":return "unknown-large.txt";
            case "opaque:slow":return "slow.txt";
            default:throw new FileNotFoundException(id);
        }
    }
    private boolean folder(String id) { return id.equals("tree:root") || id.equals("opaque:folder") || id.equals("opaque:empty") || id.equals("opaque:denied"); }
    private MatrixCursor cursor(String[] projection) {
        return new MatrixCursor(projection!=null?projection:new String[]{Document.COLUMN_DOCUMENT_ID,Document.COLUMN_DISPLAY_NAME,Document.COLUMN_MIME_TYPE,Document.COLUMN_FLAGS,Document.COLUMN_SIZE,Document.COLUMN_LAST_MODIFIED});
    }
    private byte[] data(String id) {
        if(id.equals("opaque:large")) { byte[] bytes=new byte[256*1024+1];java.util.Arrays.fill(bytes,(byte)'a');return bytes; }
        return (id.equals("opaque:child")?"tree child 中文\n":"provider notes 🔑\n").getBytes(StandardCharsets.UTF_8);
    }
    private void document(MatrixCursor cursor,String id) throws FileNotFoundException {
        Map<String,Object> values=new HashMap<>();values.put(Document.COLUMN_DOCUMENT_ID,id);values.put(Document.COLUMN_DISPLAY_NAME,name(id));
        values.put(Document.COLUMN_MIME_TYPE,folder(id)?Document.MIME_TYPE_DIR:"text/plain");values.put(Document.COLUMN_FLAGS,0);
        values.put(Document.COLUMN_SIZE,folder(id)||id.equals("opaque:large")||id.equals("opaque:slow")?null:(long)data(id).length);
        values.put(Document.COLUMN_LAST_MODIFIED,null);values.put(Document.COLUMN_SUMMARY,"active="+slowStreams.get()+";natural="+slowNaturalEnds.get());row(cursor,values);
    }
    @Override public Cursor queryDocument(String id,String[] projection) throws FileNotFoundException {
        MatrixCursor cursor=cursor(projection);document(cursor,id);
        if(id.equals("opaque:folder") && listingMode.equals("error-metadata")) {
            Bundle extras=new Bundle();extras.putString(DocumentsContract.EXTRA_ERROR,"Fixture cloud offline");cursor.setExtras(extras);
        }
        return cursor;
    }
    @Override public Cursor queryChildDocuments(String parent,String[] projection,String sort) throws FileNotFoundException {
        if(parent.equals("opaque:folder") && !listingMode.equals("ready") && !listingMode.equals("error-metadata")) {
            pendingQueries.incrementAndGet();
            MatrixCursor pending=new MatrixCursor(projection!=null?projection:new String[]{Document.COLUMN_DOCUMENT_ID,
                Document.COLUMN_DISPLAY_NAME,Document.COLUMN_MIME_TYPE,Document.COLUMN_FLAGS,Document.COLUMN_SIZE,Document.COLUMN_LAST_MODIFIED}) {
                @Override public void close() { if(!isClosed()) pendingClosed.incrementAndGet();super.close(); }
            };
            Bundle extras=new Bundle();
            if(listingMode.equals("error-bad-type")) extras.putInt(DocumentsContract.EXTRA_ERROR,7);
            else if(listingMode.equals("loading-bad-type")) extras.putString(DocumentsContract.EXTRA_LOADING,"unknown");
            else if(listingMode.startsWith("error")) extras.putString(DocumentsContract.EXTRA_ERROR,"Fixture cloud offline");
            else extras.putBoolean(DocumentsContract.EXTRA_LOADING,true);
            if(listingMode.endsWith("partial")) document(pending,"opaque:child");
            pending.setExtras(extras);
            pending.setNotificationUri(getContext().getContentResolver(),DocumentsContract.buildChildDocumentsUri(
                "com.hanxi.ghosterm.test.browser",parent));
            return pending;
        }
        MatrixCursor cursor=cursor(projection);
        if(parent.equals("opaque:denied")) throw new FileNotFoundException("Fixture directory denied");
        if(parent.equals("tree:root")) for(String id:new String[]{"opaque:folder","opaque:empty","opaque:denied","opaque:note","opaque:large","opaque:slow"}) document(cursor,id);
        else if(parent.equals("opaque:folder")) document(cursor,"opaque:child");
        else if(!parent.equals("opaque:empty")) throw new FileNotFoundException(parent);
        return cursor;
    }
    @Override public boolean isChildDocument(String parent,String id) {
        try { name(id);return parent.equals("tree:root")&&!id.equals(parent) || parent.equals("opaque:folder")&&id.equals("opaque:child"); }
        catch(FileNotFoundException error) { return false; }
    }
    @Override public ParcelFileDescriptor openDocument(String id,String mode,CancellationSignal signal) throws FileNotFoundException {
        name(id);if(!mode.equals("r") || folder(id)) throw new FileNotFoundException("Read-only fixture");
        if(signal!=null) signal.throwIfCanceled();
        try {
            if(id.equals("opaque:large")||id.equals("opaque:slow")) {
                ParcelFileDescriptor[] pipe=ParcelFileDescriptor.createPipe();
                boolean slow=id.equals("opaque:slow");
                if(slow) slowStreams.incrementAndGet();
                new Thread(()-> {
                    try(ParcelFileDescriptor.AutoCloseOutputStream output=new ParcelFileDescriptor.AutoCloseOutputStream(pipe[1])) {
                        // Keep the pipe open for four seconds unless the app closes its reader.
                        // EPIPE then proves cancellation released the descriptor, not just the UI result.
                        if(slow) {
                            for(int i=0;i<80;i++) { output.write('x');Thread.sleep(50); }
                            slowNaturalEnds.incrementAndGet();
                        }
                        output.write(data(id));
                    } catch(IOException|InterruptedException ignored) { /* reader may cancel or reject over-limit data */ }
                    finally { if(slow) slowStreams.decrementAndGet(); }
                },"browser-fixture-writer").start();
                return pipe[0];
            }
            File file=new File(directory,id.substring(id.indexOf(':')+1));
            try(FileOutputStream output=new FileOutputStream(file)) { output.write(data(id)); }
            return ParcelFileDescriptor.open(file,ParcelFileDescriptor.MODE_READ_ONLY);
        } catch(IOException failure) { throw new FileNotFoundException(failure.toString()); }
    }
}
