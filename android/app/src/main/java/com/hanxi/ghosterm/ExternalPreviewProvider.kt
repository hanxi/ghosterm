package com.hanxi.ghosterm

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import android.webkit.MimeTypeMap
import java.util.Locale

/** Framework URI grants are checked before entry; this provider is not exported.
 * No writes, persistable grants, directory roots or caller-supplied filesystem paths. */
class ExternalPreviewProvider : ContentProvider() {
    private val cache get()=(checkNotNull(context).applicationContext as GhostermApplication).externalPreviews
    override fun onCreate()=true
    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor=cache.open(uri,mode)
    override fun getType(uri: Uri): String {
        val name=cache.info(uri).first
        return MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substringAfterLast('.',"").lowercase(Locale.ROOT)) ?: "application/octet-stream"
    }
    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor {
        val info=cache.info(uri)
        val columns=projection ?: arrayOf(OpenableColumns.DISPLAY_NAME,OpenableColumns.SIZE)
        return MatrixCursor(columns).apply { addRow(columns.map { when(it) {
            OpenableColumns.DISPLAY_NAME->info.first;OpenableColumns.SIZE->info.second;else->null
        } }) }
    }
    override fun insert(uri: Uri, values: ContentValues?): Uri=throw UnsupportedOperationException("只读预览")
    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?)=throw UnsupportedOperationException("只读预览")
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?)=throw UnsupportedOperationException("只读预览")
}
