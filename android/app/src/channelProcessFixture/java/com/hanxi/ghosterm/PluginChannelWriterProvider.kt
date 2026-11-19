package com.hanxi.ghosterm

import android.content.ContentProvider
import android.content.ContentValues
import android.net.Uri
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.os.Process
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import java.io.File

/** Included only in explicitly requested debug test APKs. */
class PluginChannelWriterProvider : ContentProvider() {
    private var heldDirectory: ParcelFileDescriptor? = null

    override fun onCreate(): Boolean = true

    @Synchronized
    override fun call(method: String, arg: String?, extras: Bundle?): Bundle {
        require(arg == null)
        val application = checkNotNull(context)
        when (method) {
            "hold-cache-lock" -> {
                check(heldDirectory == null)
                val directory = LocalPublicationFile.directory(File(application.filesDir, "plugin-channel-policy"), "")
                try {
                    check(PluginInstallFiles.lock(directory.fd) == 0)
                    heldDirectory = directory
                } finally {
                    if (heldDirectory !== directory) directory.close()
                }
                return Bundle()
            }
            "release-cache-lock" -> {
                val directory = checkNotNull(heldDirectory)
                heldDirectory = null
                directory.close()
                return Bundle()
            }
        }
        require(method == "commit")
        val input = requireNotNull(extras)
        val store = checkNotNull(PluginChannelRefresh.configured(application)).store
        val policy = store.refresh(
            requireNotNull(input.getByteArray("statement")),
            requireNotNull(input.getByteArray("signature")),
            requireNotNull(input.getByteArray("policy")),
            (System.currentTimeMillis() / 1000).toULong(),
        )
        return Bundle().apply {
            putInt("pid", Process.myPid())
            putString("generation", policy.generation.toString())
            putString("statementSHA256", policy.statementSHA256)
        }
    }

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?,
        selectionArgs: Array<out String>?, sortOrder: String?) = throw UnsupportedOperationException()
    override fun getType(uri: Uri): String? = null
    override fun insert(uri: Uri, values: ContentValues?) = throw UnsupportedOperationException()
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = throw UnsupportedOperationException()
    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) =
        throw UnsupportedOperationException()
}
