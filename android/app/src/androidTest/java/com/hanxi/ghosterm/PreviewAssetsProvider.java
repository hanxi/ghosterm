package com.hanxi.ghosterm;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.system.Os;
import android.system.OsConstants;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Real SAF protocol in the test APK. ContentProvider is deliberate: the platform
 * DocumentsProvider final openAssetFile wrapper cannot represent asset segments. */
public final class PreviewAssetsProvider extends ContentProvider {
    private static final String AUTH="com.hanxi.ghosterm.test.previewassets";
    private static final String ROOT="asset:root";
    private static final String[] IDS={"segment","positioned","zero","short","pipe","error","delayed-error","pipe-offset","special","size-mismatch","invalid-range","stale-size","empty-error","finite-pipe","finite-extra"};
    private static final byte[] BODY="片段 UTF-8 🔑\n".getBytes(StandardCharsets.UTF_8);
    private static final byte[] PREFIX="NOT-THE-DOCUMENT\u0000".getBytes(StandardCharsets.UTF_8);
    private final java.util.concurrent.atomic.AtomicInteger activeReaders=new java.util.concurrent.atomic.AtomicInteger();
    private File directory;
    private volatile java.util.concurrent.CountDownLatch delayedGate=new java.util.concurrent.CountDownLatch(0);
    private volatile boolean delayedSent;

    @Override public boolean onCreate() {
        directory=new File(getContext().getFilesDir(),"preview-assets-fixture");return directory.isDirectory()||directory.mkdirs();
    }
    private static void row(MatrixCursor cursor,Map<String,Object> values) {
        Object[] cells=new Object[cursor.getColumnCount()];for(int i=0;i<cells.length;i++)cells[i]=values.get(cursor.getColumnNames()[i]);cursor.addRow(cells);
    }
    private static String id(Uri uri) {
        if(!AUTH.equals(uri.getAuthority()))throw new IllegalArgumentException("Unexpected authority");
        if(DocumentsContract.isTreeUri(uri)&&!ROOT.equals(DocumentsContract.getTreeDocumentId(uri)))throw new IllegalArgumentException("Outside tree");
        return DocumentsContract.getDocumentId(uri);
    }
    private static String kind(String id) {
        if(id.equals(ROOT))return "root";
        if(!id.startsWith("asset:"))throw new IllegalArgumentException("Unknown ID");
        String value=id.substring(6);if(!Arrays.asList(IDS).contains(value))throw new IllegalArgumentException("Unknown ID");return value;
    }
    private static Long size(String value) {
        switch(value) {
            case "root":case "pipe":case "error":case "short":return null;
            case "zero":case "special":case "empty-error":return 0L;
            case "invalid-range":return 17L;
            case "stale-size":return 4L;
            case "delayed-error":case "pipe-offset":case "finite-pipe":case "finite-extra":return 4L;
            case "size-mismatch":return (long)BODY.length+1;
            default:return (long)BODY.length;
        }
    }
    private void document(MatrixCursor cursor,String id) {
        String value=kind(id);Map<String,Object> values=new HashMap<>();
        values.put(Document.COLUMN_DOCUMENT_ID,id);values.put(Document.COLUMN_DISPLAY_NAME,value.equals("root")?"Ghosterm Preview Assets":value+".txt");
        values.put(Document.COLUMN_MIME_TYPE,value.equals("root")?Document.MIME_TYPE_DIR:"text/plain");values.put(Document.COLUMN_FLAGS,0);
        values.put(Document.COLUMN_SUMMARY,"active="+activeReaders.get());values.put(Document.COLUMN_SIZE,size(value));values.put(Document.COLUMN_LAST_MODIFIED,null);row(cursor,values);
    }
    @Override public Cursor query(Uri uri,String[] projection,String selection,String[] args,String sort) {
        if(uri.getPathSegments().size()>0&&uri.getPathSegments().get(0).equals("root")) {
            MatrixCursor c=new MatrixCursor(projection!=null?projection:new String[]{Root.COLUMN_ROOT_ID,Root.COLUMN_DOCUMENT_ID,Root.COLUMN_TITLE,Root.COLUMN_FLAGS,Root.COLUMN_MIME_TYPES});
            Map<String,Object> values=new HashMap<>();values.put(Root.COLUMN_ROOT_ID,"asset-root");values.put(Root.COLUMN_DOCUMENT_ID,ROOT);
            values.put(Root.COLUMN_TITLE,"Ghosterm Preview Assets");values.put(Root.COLUMN_FLAGS,Root.FLAG_SUPPORTS_IS_CHILD);values.put(Root.COLUMN_MIME_TYPES,"*/*");row(c,values);return c;
        }
        String id=id(uri);
        MatrixCursor c=new MatrixCursor(projection!=null?projection:new String[]{Document.COLUMN_DOCUMENT_ID,Document.COLUMN_DISPLAY_NAME,Document.COLUMN_MIME_TYPE,Document.COLUMN_FLAGS,Document.COLUMN_SIZE,Document.COLUMN_LAST_MODIFIED});
        if(uri.getLastPathSegment().equals("children")) {
            if(!ROOT.equals(id))throw new IllegalArgumentException("Not a directory");
            for(String value:IDS)document(c,"asset:"+value);
        } else document(c,id);
        return c;
    }
    @Override public String getType(Uri uri) {
        if(DocumentsContract.isTreeUri(uri)&&uri.getPathSegments().size()==2) {
            if(!ROOT.equals(DocumentsContract.getTreeDocumentId(uri)))throw new IllegalArgumentException("Outside tree");
            return Document.MIME_TYPE_DIR;
        }
        if(!uri.getPathSegments().isEmpty()&&uri.getPathSegments().get(0).equals("root"))return "vnd.android.document/root";
        return kind(id(uri)).equals("root")?Document.MIME_TYPE_DIR:"text/plain";
    }
    @Override public Bundle call(String method,String arg,Bundle extras) {
        if(method.equals("preview:release")) { delayedGate.countDown();return Bundle.EMPTY; }
        if(method.equals("preview:state")) { Bundle result=new Bundle();result.putBoolean("sent",delayedSent);return result; }
        if(method.equals("android:isChildDocument")) {
            Uri parent=extras.getParcelable("uri"),child=extras.getParcelable("android.content.extra.TARGET_URI");
            Bundle result=new Bundle();result.putBoolean("result",parent!=null&&child!=null&&id(parent).equals(ROOT)&&!kind(id(child)).equals("root"));return result;
        }
        return super.call(method,arg,extras);
    }
    @Override public AssetFileDescriptor openAssetFile(Uri uri,String mode,CancellationSignal signal) throws FileNotFoundException {
        if(!mode.equals("r"))throw new FileNotFoundException("Read-only fixture");
        if(signal!=null)signal.throwIfCanceled();String value=kind(id(uri));
        try {
            if(Arrays.asList("pipe","error","delayed-error","pipe-offset","empty-error","finite-pipe","finite-extra").contains(value)) {
                ParcelFileDescriptor[] pipes=ParcelFileDescriptor.createReliablePipe();
                java.util.concurrent.CountDownLatch gate=new java.util.concurrent.CountDownLatch(value.equals("delayed-error")||value.equals("empty-error")?1:0);
                if(value.equals("delayed-error")||value.equals("empty-error")) { delayedSent=false;delayedGate=gate; }
                new Thread(()-> {
                    try {
                        byte[] content=(value.equals("finite-extra")?"pipe-tail":value.equals("empty-error")?"":value.equals("delayed-error")?"late":value.equals("pipe-offset")||value.equals("finite-pipe")?"pipe":"normal pipe\n").getBytes(StandardCharsets.UTF_8);
                        try(FileOutputStream out=new FileOutputStream(pipes[1].getFileDescriptor())) {
                            out.write(content);out.flush();
                            if(value.equals("delayed-error")||value.equals("empty-error")) { delayedSent=true;gate.await(10,java.util.concurrent.TimeUnit.SECONDS); }
                            if(value.equals("error")||value.equals("delayed-error")||value.equals("empty-error"))pipes[1].closeWithError("Fixture preview producer failed");
                            else pipes[1].close();
                        }
                    } catch(Exception error) { try { pipes[1].close(); } catch(IOException ignored) {} }
                },"preview-asset-pipe").start();
                return new AssetFileDescriptor(pipes[0],value.equals("pipe-offset")?1:0,value.equals("empty-error")?0:value.equals("delayed-error")||value.equals("pipe-offset")||(value.equals("finite-pipe")||value.equals("finite-extra"))?4:-1);
            }
            if(value.equals("special"))return new AssetFileDescriptor(ParcelFileDescriptor.open(new File("/dev/null"),ParcelFileDescriptor.MODE_READ_ONLY),0,0);
            if(value.equals("root"))throw new FileNotFoundException("Not a file");
            File file=new File(directory,UUID.randomUUID().toString());
            try(FileOutputStream out=new FileOutputStream(file)) {
                if(value.equals("invalid-range"))out.write("negative metadata".getBytes(StandardCharsets.UTF_8));
                else if(value.equals("stale-size"))out.write("abc".getBytes(StandardCharsets.UTF_8));
                else {
                out.write(PREFIX);out.write(value.equals("short")?new byte[]{'x','y'}:BODY);
                if(!value.equals("short"))out.write("\u0000NOT-THE-DOCUMENT-TAIL".getBytes(StandardCharsets.UTF_8));
                }
            }
            ParcelFileDescriptor fd=ParcelFileDescriptor.open(file,ParcelFileDescriptor.MODE_READ_ONLY,
                new android.os.Handler(android.os.Looper.getMainLooper()),error->activeReaders.decrementAndGet());
            activeReaders.incrementAndGet();
            // Source bytes are owned by the fd from this point; no fixture files accumulate.
            if(!file.delete()) { fd.close();throw new IOException("Unable to unlink fixture asset"); }
            if(value.equals("invalid-range"))return new AssetFileDescriptor(fd,0,-2);
            if(value.equals("stale-size"))return new AssetFileDescriptor(fd,0,-1);
            if(value.equals("positioned"))Os.lseek(fd.getFileDescriptor(),5,OsConstants.SEEK_SET);
            return new AssetFileDescriptor(fd,PREFIX.length,value.equals("zero")?0:value.equals("short")?20:BODY.length);
        } catch(IOException|android.system.ErrnoException error) { throw new FileNotFoundException(error.toString()); }
    }
    @Override public AssetFileDescriptor openAssetFile(Uri uri,String mode) throws FileNotFoundException { return openAssetFile(uri,mode,null); }
    @Override public Uri insert(Uri uri,ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri,ContentValues values,String selection,String[] args) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri,String selection,String[] args) { throw new UnsupportedOperationException(); }
}
