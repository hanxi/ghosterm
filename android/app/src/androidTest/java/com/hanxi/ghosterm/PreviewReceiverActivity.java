package com.hanxi.ghosterm;

import android.app.Activity;
import android.os.Bundle;
import android.net.Uri;
import java.io.InputStream;

/** Test-APK receiver: a real different UID observes the URI grant, not the target process. */
public final class PreviewReceiverActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Bundle result=new Bundle();
        String requestedReport=getIntent().getStringExtra("report");
        final String report=requestedReport!=null?requestedReport:java.util.UUID.randomUUID().toString();
        Uri uri=getIntent().getData();
        result.putString("uri",uri!=null?uri.toString():null);result.putString("report",report);
        try {
            result.putInt("uid",android.os.Process.myUid());
            if(getIntent().getBooleanExtra("hold",false)) {
                android.os.ParcelFileDescriptor descriptor;
                // An unstable client deliberately avoids Android killing the receiver as
                // a stable provider dependent when the producer dies. The descriptor is
                // retained after releasing this client, exactly the boundary under test.
                try(android.content.ContentProviderClient client=getContentResolver().acquireUnstableContentProviderClient(uri)) {
                    if(client==null) throw new IllegalStateException("provider unavailable");
                    descriptor=client.openFile(uri,"r",null);
                }
                PreviewReceiverControl.held.put(report,descriptor);
                PreviewReceiverControl.heldLengths.put(report,descriptor.getStatSize());
                byte[] bytes=new byte[5];
                int count=android.system.Os.pread(descriptor.getFileDescriptor(),bytes,0,5,0);
                result.putByteArray("bytes",bytes);result.putBoolean("held",count==5);
            } else try(InputStream input=getContentResolver().openInputStream(uri)) {
                java.io.ByteArrayOutputStream bytes=new java.io.ByteArrayOutputStream();
                byte[] buffer=new byte[32*1024];int count;long total=0;
                java.security.MessageDigest digest=java.security.MessageDigest.getInstance("SHA-256");
                while((count=input.read(buffer))!=-1) {
                    total+=count;if(total>16L*1024*1024) throw new java.io.IOException("fixture read limit");
                    digest.update(buffer,0,count);
                    int prefix=Math.min(count,4096-bytes.size());if(prefix>0) bytes.write(buffer,0,prefix);
                }
                result.putByteArray("bytes",bytes.toByteArray());result.putLong("count",total);
                StringBuilder hash=new StringBuilder();for(byte value:digest.digest()) hash.append(String.format(java.util.Locale.ROOT,"%02x",value&255));
                result.putString("sha256",hash.toString());
            }
            boolean denied=false;
            try(android.os.ParcelFileDescriptor ignored=getContentResolver().openFileDescriptor(uri,"rw")) { }
            catch(Exception expected) { denied=true; }
            result.putBoolean("writeDenied",denied);
            result.putBoolean("readOnlyFlags",(getIntent().getFlags() & (android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION | android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION)) == android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch(Exception error) { result.putString("error",error.toString()); }
        PreviewReceiverControl.reports.put(report,result);PreviewReceiverControl.latest=new Bundle(result);
        finish();
    }
}
