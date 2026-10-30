package com.hanxi.ghosterm;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Test APK only: ephemeral SSH material is served through a pipe, never a device file. */
public final class IdentityDocumentsProvider extends DocumentsProvider {
    private static volatile byte[] material;
    private static volatile CountDownLatch release = new CountDownLatch(1);
    private static final AtomicInteger reads = new AtomicInteger(), writes = new AtomicInteger(), closed = new AtomicInteger();
    @Override public boolean onCreate() { return true; }
    static synchronized Bundle fixture(String method, Bundle extras) {
        Bundle result = new Bundle();
        if (method.equals("fixture-configure")) {
            release.countDown();
            byte[] before = material; material = extras.getByteArray("material");
            if (before != null) Arrays.fill(before,(byte)0);
            release = new CountDownLatch(1); reads.set(0); writes.set(0); closed.set(0);
        } else if (method.equals("fixture-clear")) {
            release.countDown(); byte[] before = material; material = null;
            if (before != null) Arrays.fill(before,(byte)0);
        } else if (method.equals("fixture-release")) release.countDown();
        result.putInt("reads",reads.get()); result.putInt("writes",writes.get()); result.putInt("closed",closed.get());
        return result;
    }
    @Override public Cursor queryRoots(String[] ignored) {
        MatrixCursor c = new MatrixCursor(new String[] {Root.COLUMN_ROOT_ID,Root.COLUMN_DOCUMENT_ID,Root.COLUMN_TITLE,Root.COLUMN_FLAGS,Root.COLUMN_MIME_TYPES});
        c.addRow(new Object[] {"keys","root","Ghosterm Test Keys",Root.FLAG_SUPPORTS_IS_CHILD,"application/octet-stream"});
        return c;
    }
    private MatrixCursor cursor() { return new MatrixCursor(new String[] {Document.COLUMN_DOCUMENT_ID,Document.COLUMN_DISPLAY_NAME,
        Document.COLUMN_MIME_TYPE,Document.COLUMN_FLAGS,Document.COLUMN_SIZE,Document.COLUMN_SUMMARY}); }
    private void row(MatrixCursor c,String id) {
        boolean root = id.equals("root");
        c.addRow(new Object[] {id,root?"Ghosterm Test Keys":id+".pem",root?Document.MIME_TYPE_DIR:"application/octet-stream",
            root?0:Document.FLAG_SUPPORTS_WRITE,0L,"writes="+writes.get()});
    }
    @Override public Cursor queryDocument(String id,String[] ignored) { MatrixCursor c=cursor(); row(c,id); return c; }
    @Override public Cursor queryChildDocuments(String id,String[] ignored,String sort) {
        MatrixCursor c=cursor();
        for(String key:new String[]{"ssh-key","slow-key","invalid","oversize","empty","denied"}) row(c,key);
        return c;
    }
    @Override public boolean isChildDocument(String parent,String id) { return parent.equals("root") && !id.equals("root"); }
    @Override public ParcelFileDescriptor openDocument(String id,String mode,CancellationSignal signal) throws FileNotFoundException {
        if (!mode.equals("r")) { writes.incrementAndGet(); throw new FileNotFoundException("Fixture document is unchanged"); }
        reads.incrementAndGet();
        if (id.equals("denied")) throw new SecurityException("Fixture key read denied");
        byte[] payload;
        if (id.equals("invalid")) payload = new byte[]{65,66,67};
        else if (id.equals("empty")) payload = new byte[0];
        else if (id.equals("oversize")) { payload = new byte[1024*1024+1]; Arrays.fill(payload,(byte)65); }
        else { byte[] value=material; if(value==null) throw new FileNotFoundException("SSH fixture is not configured"); payload=value.clone(); }
        try {
            ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
            CountDownLatch barrier=release;
            new Thread(() -> {
                try (ParcelFileDescriptor.AutoCloseOutputStream out=new ParcelFileDescriptor.AutoCloseOutputStream(pipe[1])) {
                    if (id.equals("slow-key")) {
                        StructPollfd p=new StructPollfd(); p.fd=pipe[1].getFileDescriptor(); p.events=(short)OsConstants.POLLOUT;
                        while (!barrier.await(20,TimeUnit.MILLISECONDS)) {
                            Os.poll(new StructPollfd[]{p},0);
                            if ((p.revents & (OsConstants.POLLERR|OsConstants.POLLHUP))!=0) { closed.incrementAndGet(); return; }
                        }
                    }
                    out.write(payload);
                } catch(Exception ignored) { closed.incrementAndGet(); }
                finally { Arrays.fill(payload,(byte)0); }
            },"identity-fixture").start();
            return pipe[0];
        } catch(IOException failure) { Arrays.fill(payload,(byte)0); throw new FileNotFoundException("Fixture pipe unavailable"); }
    }
}
