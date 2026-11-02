package com.hanxi.ghosterm;

import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.provider.DocumentsContract;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.hanxi.ghosterm.data.SafDocumentQuery;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Local provider retains the exact cursors, so GC cannot impersonate explicit
 * ownership release. Real cross-process/UI behavior is covered by FilesSafTest. */
@RunWith(AndroidJUnit4.class)
public final class SafQueryResourceTest {
    @Test public void partialAndErrorCursorsCloseBeforeReturningOrThrowing() {
        for(boolean error:new boolean[]{false,true}) {
            MatrixCursor pending=new MatrixCursor(new String[]{"name"});
            Bundle extras=new Bundle();
            if(error)extras.putString(DocumentsContract.EXTRA_ERROR,"Fixture query failed");
            else extras.putBoolean(DocumentsContract.EXTRA_LOADING,true);
            pending.setExtras(extras);
            MatrixCursor ready=new MatrixCursor(new String[]{"name"});ready.addRow(new Object[]{"complete"});
            ContentProvider provider=new ContentProvider() {
                int queries;
                @Override public boolean onCreate() { return true; }
                @Override public Cursor query(Uri uri,String[] columns,String selection,String[] args,String sort) { return queries++==0?pending:ready; }
                @Override public String getType(Uri uri) { return null; }
                @Override public Uri insert(Uri uri,ContentValues values) { throw new UnsupportedOperationException(); }
                @Override public int delete(Uri uri,String where,String[] args) { throw new UnsupportedOperationException(); }
                @Override public int update(Uri uri,ContentValues values,String where,String[] args) { throw new UnsupportedOperationException(); }
            };
            ProviderInfo info=new ProviderInfo();info.authority="com.hanxi.ghosterm.localquery";
            provider.attachInfo(InstrumentationRegistry.getInstrumentation().getTargetContext(),info);
            try {
                Cursor result=SafDocumentQuery.INSTANCE.read(ContentResolver.wrap(provider),Uri.parse("content://"+info.authority+"/children"),new String[]{"name"},new CancellationSignal());
                try {
                    assertFalse("Provider error must not return a successful cursor",error);
                    assertTrue("Partial cursor must close before returning completed query",pending.isClosed());
                    assertFalse("Completed cursor ownership belongs to caller",result.isClosed());
                    assertTrue(result.moveToFirst());assertEquals("complete",result.getString(0));
                } finally { result.close(); }
            } catch(IllegalStateException failure) {
                assertTrue("Only the fixture error may fail the query",error);
                assertTrue(failure.getMessage().contains("Fixture query failed"));
                assertTrue("Errored cursor must close before throwing",pending.isClosed());
            } finally { pending.close();ready.close(); }
        }
    }
}
