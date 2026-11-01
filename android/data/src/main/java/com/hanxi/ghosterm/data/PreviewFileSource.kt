package com.hanxi.ghosterm.data

import com.hanxi.ghosterm.domain.*

/** Raw bounded copies for explicit external previews; never a destination publication. */
interface PreviewFileSource {
    suspend fun snapshot(location: FileLocation, entry: BrowserEntry, store: TransferStagingStore,
        progress: (Long, Long?) -> Unit = { _, _ -> }): TransferStagingStore.Snapshot
}
