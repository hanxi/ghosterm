package com.hanxi.ghosterm;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.*;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import java.io.*;
import java.util.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Writable, fault-injectable SAF fixture in the TEST APK, behind real tree grants. */
public final class PublicationDocumentsProvider extends DocumentsProvider {
    private static PublicationDocumentsProvider instance;
    private File directory;
    private final Map<String,Entry> entries=new java.util.concurrent.ConcurrentHashMap<>();
    private volatile String mode="normal";
    private volatile CountDownLatch release=new CountDownLatch(0);
    private volatile String latest;
    private final AtomicInteger creates=new AtomicInteger(),writes=new AtomicInteger(),reads=new AtomicInteger(),deletes=new AtomicInteger(),active=new AtomicInteger(),closedWrites=new AtomicInteger(),writeErrors=new AtomicInteger();
    private Handler handler;
    private static final class Entry {
        final String id,name,parent,mode;final File file;boolean directory;
        volatile CountDownLatch closed=new CountDownLatch(0);
        Entry(String id,String name,String parent,String mode,File file) { this.id=id;this.name=name;this.parent=parent;this.mode=mode;this.file=file; }
    }
    @Override public boolean onCreate() {
        instance=this;directory=new File(getContext().getFilesDir(),"publication-fixture");directory.mkdirs();
        HandlerThread thread=new HandlerThread("publication-close");thread.start();handler=new Handler(thread.getLooper());
        reset("normal");return true;
    }
    private void reset(String next) {
        release.countDown();mode=next;release=new CountDownLatch(1);entries.clear();latest=null;
        for(File f:Objects.requireNonNull(directory.listFiles()))f.delete();
        creates.set(0);writes.set(0);reads.set(0);deletes.set(0);closedWrites.set(0);writeErrors.set(0);
        try { Entry keep=new Entry("opaque:keep","keep.txt","pub:root","normal",new File(directory,"keep"));
            try(FileOutputStream out=new FileOutputStream(keep.file)) { out.write("original".getBytes(java.nio.charset.StandardCharsets.UTF_8)); }
            entries.put(keep.id,keep);
        } catch(IOException error) { throw new IllegalStateException(error); }
    }
    static Bundle fixture(String action,Bundle input) {
        PublicationDocumentsProvider self=instance;
        if(self==null) throw new IllegalStateException("Documents provider not initialized");
        if(action.equals("reset"))self.reset(input.getString("mode","normal"));
        else if(action.equals("release"))self.release.countDown();
        else if(!action.equals("state"))throw new IllegalArgumentException(action);
        Bundle result=new Bundle();result.putInt("creates",self.creates.get());result.putInt("writes",self.writes.get());result.putInt("reads",self.reads.get());
        result.putInt("deletes",self.deletes.get());result.putInt("active",self.active.get());result.putInt("closed",self.closedWrites.get());result.putInt("writeErrors",self.writeErrors.get());result.putString("latest",self.latest);
        try {
            result.putByteArray("keep",java.nio.file.Files.readAllBytes(self.entries.get("opaque:keep").file.toPath()));
            Entry e=self.entries.get(self.latest==null?"":self.latest);if(e!=null&&!e.directory)result.putByteArray("bytes",java.nio.file.Files.readAllBytes(e.file.toPath()));
        } catch(IOException error) { throw new IllegalStateException(error); }
        return result;
    }
    private static void row(MatrixCursor c,Map<String,Object> values) {
        Object[] cells=new Object[c.getColumnCount()];for(int i=0;i<cells.length;i++)cells[i]=values.get(c.getColumnNames()[i]);c.addRow(cells);
    }
    @Override public Cursor queryRoots(String[] projection) {
        MatrixCursor c=new MatrixCursor(projection!=null?projection:new String[]{Root.COLUMN_ROOT_ID,Root.COLUMN_DOCUMENT_ID,Root.COLUMN_TITLE,Root.COLUMN_FLAGS,Root.COLUMN_MIME_TYPES});
        Map<String,Object> v=new HashMap<>();v.put(Root.COLUMN_ROOT_ID,"publication");v.put(Root.COLUMN_DOCUMENT_ID,"pub:root");v.put(Root.COLUMN_TITLE,"Ghosterm Copy Destination");
        v.put(Root.COLUMN_FLAGS,Root.FLAG_SUPPORTS_CREATE|Root.FLAG_SUPPORTS_IS_CHILD);v.put(Root.COLUMN_MIME_TYPES,"*/*");row(c,v);return c;
    }
    private MatrixCursor cursor(String[] projection) { return new MatrixCursor(projection!=null?projection:new String[]{Document.COLUMN_DOCUMENT_ID,Document.COLUMN_DISPLAY_NAME,Document.COLUMN_MIME_TYPE,Document.COLUMN_FLAGS,Document.COLUMN_SIZE,Document.COLUMN_LAST_MODIFIED}); }
    private boolean folder(String id) { Entry e=entries.get(id);return id.equals("pub:root")||id.equals("pub:folder")||id.equals("outside")||e!=null&&e.directory; }
    private void document(MatrixCursor c,String id) throws FileNotFoundException {
        Entry e=entries.get(id);if(!folder(id)&&e==null)throw new FileNotFoundException(id);
        Map<String,Object> v=new HashMap<>();v.put(Document.COLUMN_DOCUMENT_ID,mode.equals("wrong-metadata")&&id.startsWith("opaque:new:")?"wrong-id":id);
        v.put(Document.COLUMN_DISPLAY_NAME,e!=null?e.name:(id.equals("pub:root")?"Ghosterm Copy Destination":"Nested"));
        v.put(Document.COLUMN_MIME_TYPE,folder(id)?Document.MIME_TYPE_DIR:"application/octet-stream");
        v.put(Document.COLUMN_FLAGS,folder(id)?(mode.equals("no-create")?0:Document.FLAG_DIR_SUPPORTS_CREATE):(mode.equals("no-write")?0:Document.FLAG_SUPPORTS_WRITE|(mode.equals("virtual")?Document.FLAG_VIRTUAL_DOCUMENT:0)));
        v.put(Document.COLUMN_SIZE,folder(id)||mode.equals("short")||mode.equals("extra")||mode.startsWith("pipe")?null:e.file.length());
        v.put(Document.COLUMN_LAST_MODIFIED,null);row(c,v);
    }
    @Override public Cursor queryDocument(String id,String[] projection) throws FileNotFoundException {
        MatrixCursor c=cursor(projection);document(c,id);return c;
    }
    @Override public Cursor queryChildDocuments(String parent,String[] projection,String sort) throws FileNotFoundException {
        if(mode.equals("list-fail"))throw new FileNotFoundException("Listing denied");
        MatrixCursor c=cursor(projection);
        if(parent.equals("pub:root"))document(c,"pub:folder");
        for(Entry e:entries.values())if(e.parent.equals(parent))document(c,e.id);
        if(mode.equals("duplicate"))document(c,"opaque:keep");
        if(mode.equals("too-many"))for(int i=0;i<4097;i++) {
            Map<String,Object> v=new HashMap<>();v.put(Document.COLUMN_DOCUMENT_ID,"extra:"+i);v.put(Document.COLUMN_DISPLAY_NAME,"extra-"+i);row(c,v);
        }
        if(mode.equals("directory-metadata")&&parent.equals("pub:root"))for(int i=0;i<1100;i++) {
            Map<String,Object> v=new HashMap<>();v.put(Document.COLUMN_DOCUMENT_ID,"large:"+i+":"+String.join("",Collections.nCopies(4096,"x")));
            v.put(Document.COLUMN_DISPLAY_NAME,"large-"+i);row(c,v);
        }
        return c;
    }
    @Override public boolean isChildDocument(String parent,String id) {
        if(mode.equals("directory-outside")&&id.startsWith("opaque:new:"))return false;
        Entry e=entries.get(id);
        return parent.equals("pub:root")&&(id.equals("pub:folder")||e!=null) || e!=null&&e.parent.equals(parent);
    }
    @Override public String createDocument(String parent,String mime,String name) throws FileNotFoundException {
        creates.incrementAndGet();
        if(mode.equals("return-existing"))return "opaque:keep";
        if(mode.equals("return-parent"))return parent;
        if(mode.equals("create-denied"))throw new FileNotFoundException("Create denied");
        String id="opaque:new:"+UUID.randomUUID();String actual=mode.equals("rename")?"provider-"+name:name;
        Entry e=new Entry(id,actual,mode.equals("directory-indirect")?"pub:folder":parent,mode,new File(directory,UUID.randomUUID().toString()));
        e.directory=mime.equals(Document.MIME_TYPE_DIR)&&!mode.equals("directory-file");
        try { if(!(e.directory?e.file.mkdir():e.file.createNewFile()))throw new IOException(); } catch(IOException error) { throw new FileNotFoundException(error.toString()); }
        entries.put(id,e);latest=id;
        if(mode.equals("directory-nonempty")) {
            Entry nested=new Entry("opaque:unexpected","unexpected.txt",id,mode,new File(directory,UUID.randomUUID().toString()));
            try { if(!nested.file.createNewFile())throw new IOException(); } catch(IOException error) { throw new FileNotFoundException(error.toString()); }
            entries.put(nested.id,nested);
        }
        if(mode.equals("new-nonempty"))try(FileOutputStream out=new FileOutputStream(e.file)) { out.write(9); }
        catch(IOException error) { throw new FileNotFoundException(error.toString()); }
        if(mode.equals("late-create"))waitGate();
        if(mode.equals("create-lost"))throw new FileNotFoundException("Reply lost after create");
        return id;
    }
    private void waitGate() throws FileNotFoundException {
        try { if(!release.await(10,TimeUnit.SECONDS))throw new FileNotFoundException("Fixture release timed out"); }
        catch(InterruptedException error) { Thread.currentThread().interrupt();throw new FileNotFoundException("Interrupted"); }
    }
    /** Fill the pipe then free only PIPE_BUF (4 KiB on the tested Linux device).
     * POLLOUT is now true, but a blocking 32 KiB write cannot complete until release.
     * These fixture bytes never claim to be a successful target; the test cancels it.
     */
    private static void leaveOneAtomicWriteFree(ParcelFileDescriptor[] pipe) throws IOException,android.system.ErrnoException {
        java.io.FileDescriptor writer=pipe[1].getFileDescriptor();byte[] bytes=new byte[4096];
        int flags=android.system.Os.fcntlInt(writer,android.system.OsConstants.F_GETFL,0);
        android.system.Os.fcntlInt(writer,android.system.OsConstants.F_SETFL,flags|android.system.OsConstants.O_NONBLOCK);
        try {
            while(true)try { android.system.Os.write(writer,bytes,0,bytes.length); }
            catch(android.system.ErrnoException failure) {
                if(failure.errno==android.system.OsConstants.EAGAIN)break;
                if(failure.errno!=android.system.OsConstants.EINTR)throw failure;
            }
            if(android.system.Os.read(pipe[0].getFileDescriptor(),bytes,0,bytes.length)!=bytes.length)throw new IOException("Pipe capacity fixture failed");
        } finally { android.system.Os.fcntlInt(writer,android.system.OsConstants.F_SETFL,flags); }
    }
    @Override public void deleteDocument(String id) { deletes.incrementAndGet();throw new UnsupportedOperationException("Must not blindly delete"); }
    @Override public ParcelFileDescriptor openDocument(String id,String access,CancellationSignal signal) throws FileNotFoundException {
        Entry e=entries.get(id);if(e==null)throw new FileNotFoundException(id);
        if(signal!=null)signal.throwIfCanceled();
        try {
            if(access.equals("w")) {
                writes.incrementAndGet();if(e.mode.equals("write-denied"))throw new FileNotFoundException("Write denied");
                e.closed=new CountDownLatch(1);
                if(e.mode.equals("pipe-write")||e.mode.equals("pipe-stall")) {
                    ParcelFileDescriptor[] pair=ParcelFileDescriptor.createReliablePipe();
                    if(e.mode.equals("pipe-stall"))try { leaveOneAtomicWriteFree(pair); }
                    catch(IOException|android.system.ErrnoException failure) { pair[0].close();pair[1].close();throw failure; }
                    active.incrementAndGet();
                    new Thread(()-> {
                        try(ParcelFileDescriptor.AutoCloseInputStream in=new ParcelFileDescriptor.AutoCloseInputStream(pair[0]);FileOutputStream out=new FileOutputStream(e.file)) {
                            if(e.mode.equals("pipe-stall"))waitGate();
                            byte[] bytes=new byte[4096];int n;while((n=in.read(bytes))!=-1)out.write(bytes,0,n);
                        } catch(IOException ignored) { writeErrors.incrementAndGet(); }
                        finally { closedWrites.incrementAndGet();e.closed.countDown();active.decrementAndGet(); }
                    },"publication-pipe").start();return pair[1];
                }
                return ParcelFileDescriptor.open(e.file,ParcelFileDescriptor.MODE_WRITE_ONLY,handler,error->{ closedWrites.incrementAndGet();e.closed.countDown(); });
            }
            if(!access.equals("r"))throw new FileNotFoundException("Unexpected access mode "+access);
            reads.incrementAndGet();
            try { if(!e.closed.await(3,TimeUnit.SECONDS))throw new FileNotFoundException("Writer still open"); }
            catch(InterruptedException error) { Thread.currentThread().interrupt();throw new FileNotFoundException("Interrupted"); }
            if(e.mode.equals("read-denied"))throw new FileNotFoundException("Readback denied");
            if(e.mode.equals("read-stall"))waitGate();
            if(e.mode.equals("pipe-error")) {
                ParcelFileDescriptor[] pair=ParcelFileDescriptor.createReliablePipe();
                new Thread(()-> { try { byte[] bytes=java.nio.file.Files.readAllBytes(e.file.toPath());
                    int offset=0;while(offset<bytes.length) {
                        int n=android.system.Os.write(pair[1].getFileDescriptor(),bytes,offset,bytes.length-offset);
                        if(n<=0)throw new IOException("Pipe stopped");offset+=n;
                    }
                    pair[1].closeWithError("Fixture readback failed");
                } catch(IOException|android.system.ErrnoException ignored) { try { pair[1].close(); } catch(IOException ignoredAgain) {} } },"publication-error").start();return pair[0];
            }
            if(e.mode.equals("corrupt")||e.mode.equals("short")||e.mode.equals("extra")) {
                try(RandomAccessFile file=new RandomAccessFile(e.file,"rw")) {
                    if(e.mode.equals("short"))file.setLength(Math.max(0,file.length()-1));
                    else if(e.mode.equals("extra")) { file.seek(file.length());file.write(99); }
                    else { file.seek(0);file.write(99); }
                }
            }
            return ParcelFileDescriptor.open(e.file,ParcelFileDescriptor.MODE_READ_ONLY);
        } catch(IOException|android.system.ErrnoException error) { throw new FileNotFoundException(error.toString()); }
    }
}
