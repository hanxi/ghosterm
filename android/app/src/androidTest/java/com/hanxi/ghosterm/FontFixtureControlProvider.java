package com.hanxi.ghosterm;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.content.Intent;

public final class FontFixtureControlProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }
    @Override public Bundle call(String method,String arg,Bundle extras) {
        if(method.equals("grant")) getContext().grantUriPermission("com.hanxi.ghosterm",
            DocumentsContract.buildDocumentUri(FontDocumentsProvider.AUTH,arg),Intent.FLAG_GRANT_READ_URI_PERMISSION);
        if(method.equals("revoke")) getContext().revokeUriPermission(Uri.parse("content://"+FontDocumentsProvider.AUTH),Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return FontDocumentsProvider.control(method);
    }
    @Override public Cursor query(Uri u,String[] p,String s,String[] a,String order) { return null; }
    @Override public String getType(Uri u) { return null; }
    @Override public Uri insert(Uri u,ContentValues v) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri u,ContentValues v,String s,String[] a) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri u,String s,String[] a) { throw new UnsupportedOperationException(); }
}
