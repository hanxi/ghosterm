package com.hanxi.ghosterm

import android.content.Intent
import android.content.res.AssetFileDescriptor
import android.net.Uri
import android.os.Bundle
import android.os.ParcelFileDescriptor
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Default test exercises the foreign seekable reader. The host additionally kills
 * this app after the explicit held-descriptor checkpoint, then verifies EIO + sweep. */
@RunWith(AndroidJUnit4::class)
class ExternalPreviewRestartTest {
    @Test fun foreignConsumerCannotOwnTheCacheInodeAcrossProcessDeath()=runBlocking {
        val instrumentation=InstrumentationRegistry.getInstrumentation()
        val context=instrumentation.targetContext
        val cache=(context.applicationContext as GhostermApplication).externalPreviews
        val report=UUID.randomUUID().toString()
        val source=File(context.cacheDir,"preview-restart-source-$report").apply {
            writeBytes(ByteArray(8*1024*1024).apply { "hello".toByteArray().copyInto(this);"world".toByteArray().copyInto(this,size-5) })
        }
        val ready=try { cache.prepare("restart.txt",source.length().toULong()) { store -> store.snapshot(source.length()) {
            AssetFileDescriptor(ParcelFileDescriptor.open(source,ParcelFileDescriptor.MODE_READ_ONLY),0,-1)
        } } } finally { source.delete() }
        val control=Uri.parse("content://com.hanxi.ghosterm.test.previewcontrol")
        val hold=InstrumentationRegistry.getArguments().getString("externalPreviewHold")=="true"
        try {
            context.startActivity(externalPreviewIntent(ready).setClassName("com.hanxi.ghosterm.test",
                "com.hanxi.ghosterm.PreviewReceiverActivity").putExtra("report",report).putExtra("hold",true)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            var response: Bundle?=null
            withTimeout(10_000) { while(response==null) { response=context.contentResolver.call(control,"report",report,null);delay(20) } }
            assertNull(response!!.getString("error"));assertTrue(response!!.getBoolean("held"))
            assertNotEquals(android.os.Process.myUid(),response!!.getInt("uid"))
            assertArrayEquals("hello".toByteArray(),response!!.getByteArray("bytes"))
            assertTrue(context.contentResolver.call(control,"readHeld",report,null)!!.getBoolean("readable"))
            if(!hold) {
                val tail=context.contentResolver.call(control,"readHeldTail",report,null)!!
                assertTrue(tail.getBoolean("readable"));assertArrayEquals("world".toByteArray(),tail.getByteArray("bytes"))
            }
            if(hold) {
                val token=ready.uri.pathSegments[0]
                val copy=File(context.cacheDir,"external-previews/$token")
                assertTrue(copy.setLastModified(System.currentTimeMillis()-ExternalPreviewCache.RETENTION_MILLIS-1000))
                cache.sweep();assertTrue("an active reader pins even an expired named copy",copy.exists())
                instrumentation.sendStatus(2,Bundle().apply {
                    putString("preview_report",report);putString("preview_token",token)
                })
                // The host must observe a live producer, then kill it itself. A normal
                // instrumentation exit also stops its target and is not this checkpoint.
                withTimeout(90_000) { awaitCancellation() }
            }
        } finally {
            withContext(NonCancellable) {
                context.contentResolver.call(control,"closeHeld",report,null)
                withTimeout(5000) { while(true) { try { cache.discard(ready);break } catch(_: IllegalStateException) { delay(20) } } }
            }
        }
    }
}
