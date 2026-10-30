package com.hanxi.ghosterm;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;

/** Test-only in-memory fixture control. Documents themselves still require the real SAF grant. */
public final class PublicationFixtureControlProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }
    @Override public Bundle call(String method,String arg,Bundle extras) {
        try {
            if(Binder.getCallingUid()!=getContext().getPackageManager().getApplicationInfo("com.hanxi.ghosterm",0).uid)
                throw new SecurityException("Only the instrumented target may configure the fixture");
        } catch(PackageManager.NameNotFoundException failure) { throw new SecurityException("Test target unavailable"); }
        // Force provider creation in this process; mutable fixture state stays test-only.
        try(android.content.ContentProviderClient client=getContext().getContentResolver().acquireContentProviderClient("com.hanxi.ghosterm.test.publication")) {
            if(client==null)throw new IllegalStateException("Missing fixture provider");
            return PublicationDocumentsProvider.fixture(method,extras);
        }
    }
    @Override public Cursor query(Uri u,String[] p,String s,String[] a,String o) { throw new UnsupportedOperationException(); }
    @Override public String getType(Uri u) { return null; }
    @Override public Uri insert(Uri u,ContentValues v) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri u,String s,String[] a) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri u,ContentValues v,String s,String[] a) { throw new UnsupportedOperationException(); }
}
