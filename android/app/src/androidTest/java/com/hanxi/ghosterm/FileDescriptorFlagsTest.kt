package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.FileDescriptorFlags
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import java.util.concurrent.Callable
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException

@RunWith(AndroidJUnit4::class)
class FileDescriptorFlagsTest {
    @Test fun statusChangesPreserveAppendAndCloseTheirTemporaryDuplicates() {
        val file=File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir,"fd-flags-${UUID.randomUUID()}")
        file.writeText("A")
        val fd=Os.open(file.path,OsConstants.O_RDWR or OsConstants.O_APPEND or OsConstants.O_CLOEXEC,0)
        try {
            val initial=FileDescriptorFlags.status(fd)
            assertEquals("status getter preserves append",OsConstants.O_APPEND,initial and OsConstants.O_APPEND)
            assertEquals(OsConstants.O_RDWR,initial and OsConstants.O_ACCMODE)
            FileDescriptorFlags.status(fd,initial or OsConstants.O_NONBLOCK)
            Os.lseek(fd,0,OsConstants.SEEK_SET);Os.write(fd,byteArrayOf(66),0,1)
            assertEquals("setting nonblocking preserves actual append behavior","AB",file.readText())
            FileDescriptorFlags.status(fd,initial)
            Os.lseek(fd,0,OsConstants.SEEK_SET);Os.write(fd,byteArrayOf(67),0,1)
            assertEquals("restoring flags preserves actual append behavior","ABC",file.readText())
            repeat(32) { FileDescriptorFlags.status(fd) }
            val matching=File("/proc/self/fd").listFiles()!!.count {
                try { Os.readlink(it.path).endsWith("/"+file.name) } catch(_:ErrnoException) { false }
            }
            assertEquals("temporary status duplicates must close",1,matching)
        } finally { Os.close(fd);file.delete() }
    }

    @Test fun nonblockingChangesTheOriginalPipeAndRestoreMakesItWaitAgain() {
        val pipe=ParcelFileDescriptor.createPipe()
        val worker=Executors.newSingleThreadExecutor()
        fun read(started:CountDownLatch)=worker.submit(Callable {
            started.countDown()
            try {
                val bytes=ByteArray(1)
                "read:${Os.read(pipe[0].fileDescriptor,bytes,0,1)}:${bytes[0]}"
            } catch(error:ErrnoException) { "errno:${error.errno}" }
        })
        try {
            val initial=FileDescriptorFlags.status(pipe[0].fileDescriptor)
            FileDescriptorFlags.status(pipe[0].fileDescriptor,initial or OsConstants.O_NONBLOCK)
            val immediate=read(CountDownLatch(0))
            val result=try { immediate.get(1500,TimeUnit.MILLISECONDS) }
                catch(_:TimeoutException) { fail("nonblocking applies to the original pipe");"unreachable" }
            assertEquals("empty nonblocking pipe returns EAGAIN","errno:${OsConstants.EAGAIN}",result)
            FileDescriptorFlags.status(pipe[0].fileDescriptor,initial)
            assertEquals("restore removes nonblocking on the original description",0,
                FileDescriptorFlags.status(pipe[0].fileDescriptor) and OsConstants.O_NONBLOCK)
            val started=CountDownLatch(1);val waiting=read(started)
            assertTrue(started.await(2,TimeUnit.SECONDS));SystemClock.sleep(150)
            assertFalse("restored pipe blocks until real input",waiting.isDone)
            Os.write(pipe[1].fileDescriptor,byteArrayOf(90),0,1)
            assertEquals("read:1:90",waiting.get(2,TimeUnit.SECONDS))
        } finally {
            // Release a reader even when a mutation left the first read blocking.
            try { Os.write(pipe[1].fileDescriptor,byteArrayOf(9),0,1) } catch(_:Exception) { }
            pipe[1].close();worker.shutdown()
            val stopped=worker.awaitTermination(3,TimeUnit.SECONDS)
            pipe[0].close()
            assertTrue("pipe reader must terminate",stopped)
        }
    }

    @Test fun descriptorFlagsObserveTheOriginalSlotAndKeepOtherDuplicatesIndependent() {
        val pipe=ParcelFileDescriptor.createPipe()
        try { ParcelFileDescriptor.dup(pipe[0].fileDescriptor).use { replacement ->
            assertEquals(OsConstants.FD_CLOEXEC,FileDescriptorFlags.descriptorFlags(pipe[0].fd) and OsConstants.FD_CLOEXEC)
            // POSIX dup2 clears CLOEXEC on this already owned target slot.
            Os.dup2(pipe[0].fileDescriptor,replacement.fd)
            assertEquals("descriptor getter observes the original slot",0,FileDescriptorFlags.descriptorFlags(replacement.fd) and OsConstants.FD_CLOEXEC)
            assertEquals("other descriptor retains CLOEXEC",OsConstants.FD_CLOEXEC,FileDescriptorFlags.descriptorFlags(pipe[0].fd) and OsConstants.FD_CLOEXEC)
        } } finally { pipe.forEach { it.close() } }
    }

    @Test fun invalidDescriptorPreservesTheOperatingSystemError() {
        var failure:ErrnoException?=null
        try { FileDescriptorFlags.descriptorFlags(-1) } catch(error:ErrnoException) { failure=error }
        assertNotNull("invalid descriptor must throw the OS error",failure)
        assertEquals(OsConstants.EBADF,failure!!.errno)
    }
}
