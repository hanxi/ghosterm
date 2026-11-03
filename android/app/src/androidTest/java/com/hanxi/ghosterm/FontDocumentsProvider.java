package com.hanxi.ghosterm;

import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsProvider;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;
import java.io.FileNotFoundException;
import java.io.InputStream;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Test APK only: public font resources through the actual DocumentsUI and reliable pipes. */
public final class FontDocumentsProvider extends DocumentsProvider {
    static final String AUTH="com.hanxi.ghosterm.test.fonts";
    private static final AtomicInteger reads=new AtomicInteger(),closed=new AtomicInteger(),writes=new AtomicInteger();
    private static volatile CountDownLatch release=new CountDownLatch(1);
    static Bundle control(String action) {
        if(action.equals("reset")) { release.countDown();release=new CountDownLatch(1);reads.set(0);closed.set(0);writes.set(0); }
        if(action.equals("release")) release.countDown();
        Bundle result=new Bundle();result.putInt("reads",reads.get());result.putInt("closed",closed.get());result.putInt("writes",writes.get());return result;
    }
    @Override public boolean onCreate() { return true; }
    @Override public Cursor queryRoots(String[] ignored) {
        MatrixCursor c=new MatrixCursor(new String[]{DocumentsContract.Root.COLUMN_ROOT_ID,DocumentsContract.Root.COLUMN_DOCUMENT_ID,
            DocumentsContract.Root.COLUMN_TITLE,DocumentsContract.Root.COLUMN_FLAGS,DocumentsContract.Root.COLUMN_MIME_TYPES});
        c.addRow(new Object[]{"fonts","root","Ghosterm Test Fonts",DocumentsContract.Root.FLAG_SUPPORTS_IS_CHILD,"*/*"});return c;
    }
    private MatrixCursor cursor() { return new MatrixCursor(new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID,DocumentsContract.Document.COLUMN_DISPLAY_NAME,
        DocumentsContract.Document.COLUMN_MIME_TYPE,DocumentsContract.Document.COLUMN_SIZE,DocumentsContract.Document.COLUMN_FLAGS}); }
    private void row(MatrixCursor c,String id) { c.addRow(new Object[]{id,id.equals("root")?"Ghosterm Test Fonts":id+".ttf",
        id.equals("root")?DocumentsContract.Document.MIME_TYPE_DIR:"font/ttf",null,0}); }
    @Override public Cursor queryDocument(String id,String[] ignored) { MatrixCursor c=cursor();row(c,id);return c; }
    @Override public Cursor queryChildDocuments(String id,String[] ignored,String sort) {
        MatrixCursor c=cursor();for(String name:new String[]{"regular","slow","error","oversize","invalid"})row(c,name);return c;
    }
    @Override public boolean isChildDocument(String parent,String id) { return parent.equals("root") && !id.equals("root"); }
    private byte[] material() throws Exception {
        Context target=getContext().createPackageContext("com.hanxi.ghosterm",0);
        int id=target.getResources().getIdentifier("source_code_pro","font","com.hanxi.ghosterm");
        try(InputStream in=target.getResources().openRawResource(id)) {
            java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream();byte[] buffer=new byte[16384];int n;
            while((n=in.read(buffer))!=-1)out.write(buffer,0,n);return out.toByteArray();
        }
    }
    @Override public ParcelFileDescriptor openDocument(String id,String mode,CancellationSignal signal) throws FileNotFoundException {
        if(!mode.equals("r")) { writes.incrementAndGet();throw new FileNotFoundException("read only font fixture"); }
        reads.incrementAndGet();
        try {
            byte[] payload=id.equals("oversize")?new byte[4*1024*1024+1]:id.equals("invalid")?new byte[]{1,2,3}:material();
            ParcelFileDescriptor[] pipe=ParcelFileDescriptor.createReliablePipe();CountDownLatch barrier=release;
            new Thread(()-> {
                try {
                    if(id.equals("slow")) {
                        StructPollfd p=new StructPollfd();p.fd=pipe[1].getFileDescriptor();p.events=(short)OsConstants.POLLOUT;
                        while(!barrier.await(20,TimeUnit.MILLISECONDS)) {
                            Os.poll(new StructPollfd[]{p},0);
                            if((p.revents&(OsConstants.POLLERR|OsConstants.POLLHUP))!=0)return;
                        }
                    }
                    ParcelFileDescriptor.AutoCloseOutputStream out=new ParcelFileDescriptor.AutoCloseOutputStream(pipe[1]);
                    out.write(payload);out.flush();
                    if(id.equals("error"))pipe[1].closeWithError("font producer failed");else out.close();
                } catch(Exception ignored) { try { pipe[1].close(); } catch(Exception unused) {} }
                finally { Arrays.fill(payload,(byte)0);closed.incrementAndGet(); }
            },"font-fixture").start();
            return pipe[0];
        } catch(Exception failure) { throw new FileNotFoundException("font fixture unavailable: "+failure); }
    }
}
