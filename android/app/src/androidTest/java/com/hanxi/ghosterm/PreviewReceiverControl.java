package com.hanxi.ghosterm;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import java.util.concurrent.ConcurrentHashMap;

/** Test-only control runs under the receiver APK UID, before and after the actual launch. */
public final class PreviewReceiverControl extends ContentProvider {
    static final ConcurrentHashMap<String,Bundle> reports=new ConcurrentHashMap<>();
    static final ConcurrentHashMap<String,android.os.ParcelFileDescriptor> held=new ConcurrentHashMap<>();
    static final ConcurrentHashMap<String,Long> heldLengths=new ConcurrentHashMap<>();
    static volatile Bundle latest;
    @Override public boolean onCreate() { return true; }
    @Override public Bundle call(String method,String arg,Bundle extras) {
        if(method.equals("clearLast")) { latest=null;return Bundle.EMPTY; }
        if(method.equals("lastReport")) return latest;
        if(method.equals("report")) return reports.remove(arg);
        if(method.equals("closeAllHeld")) {
            for(android.os.ParcelFileDescriptor descriptor:held.values()) try { descriptor.close(); } catch(java.io.IOException ignored) { }
            held.clear();heldLengths.clear();return Bundle.EMPTY;
        }
        if(method.equals("readHeld") || method.equals("readHeldTail")) {
            Bundle answer=new Bundle();answer.putInt("pid",android.os.Process.myPid());answer.putBoolean("held",held.containsKey(arg));
            try {
                android.os.ParcelFileDescriptor descriptor=held.get(arg);
                if(descriptor==null) throw new IllegalStateException("missing held descriptor");
                byte[] bytes=new byte[5];long offset=method.equals("readHeldTail")?heldLengths.get(arg)-5:0;
                int count=android.system.Os.pread(descriptor.getFileDescriptor(),bytes,0,5,offset);
                answer.putBoolean("readable",count==5);answer.putByteArray("bytes",bytes);
            } catch(Exception error) { answer.putBoolean("readable",false);answer.putString("error",error.toString()); }
            return answer;
        }
        if(method.equals("closeHeld")) {
            android.os.ParcelFileDescriptor descriptor=held.remove(arg);heldLengths.remove(arg);
            if(descriptor!=null) try { descriptor.close(); } catch(java.io.IOException error) { throw new IllegalStateException(error); }
            return Bundle.EMPTY;
        }
        if(method.equals("probe")) {
            Bundle answer=new Bundle();answer.putInt("uid",android.os.Process.myUid());
            try(java.io.InputStream ignored=getContext().getContentResolver().openInputStream(Uri.parse(arg))) { answer.putBoolean("denied",false); }
            catch(Exception expected) { answer.putBoolean("denied",true); }
            return answer;
        }
        throw new IllegalArgumentException(method);
    }
    @Override public Cursor query(Uri uri,String[] projection,String selection,String[] arguments,String sort) { throw new UnsupportedOperationException(); }
    @Override public String getType(Uri uri) { return null; }
    @Override public Uri insert(Uri uri,ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri,String selection,String[] arguments) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri,ContentValues values,String selection,String[] arguments) { throw new UnsupportedOperationException(); }
}
