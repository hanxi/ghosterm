package com.hanxi.ghosterm.data

import com.hanxi.ghosterm.bridge.RemoteFileSession
import com.hanxi.ghosterm.domain.FileEndpoint
import com.hanxi.ghosterm.domain.FileLocation

/** Caller-owned authenticated connection for a frozen endpoint. This binding does
 * not borrow a browser connection or perform authentication/reconnect. A task must
 * supply its own owner and keep it alive until all requests have actually closed. */
class RemoteFileBinding(val endpoint: FileEndpoint, val session: RemoteFileSession) {
    init {
        val identity = requireNotNull(endpoint.remote) { "远端身份缺失" }
        require(endpoint.treeUri == null && endpoint.id.isNotEmpty()) { "远端位置无效" }
        require(identity.host == session.peer.host && identity.port == session.peer.port &&
            identity.user == session.peer.user) { "连接与固定的远端身份不一致" }
    }
    fun prepareDirectory(parent: FileLocation, name: String, timeoutMillis: Int): RemoteFileSession.DirectoryRequest {
        require(parent.endpoint == endpoint) { "目录请求不属于此远端位置" }
        check(session.state().ready) { "文件连接尚未就绪或已关闭，请重新连接" }
        return session.prepareDirectory(parent.directoryId, name, timeoutMillis)
    }
}
