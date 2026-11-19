package com.hanxi.ghosterm

import android.content.ContentProvider
import android.content.ContentValues
import android.net.Uri
import android.os.Bundle
import android.os.Process

/** Included only in explicitly requested debug test APKs. */
class PluginChannelWriterProvider : ContentProvider() {
    override fun onCreate(): Boolean = true

    override fun call(method: String, arg: String?, extras: Bundle?): Bundle {
        require(method == "commit" && arg == null)
        val input = requireNotNull(extras)
        val store = checkNotNull(PluginChannelRefresh.configured(checkNotNull(context))).store
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
