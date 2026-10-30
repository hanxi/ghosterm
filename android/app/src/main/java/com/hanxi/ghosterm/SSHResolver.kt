package com.hanxi.ghosterm

import android.net.DnsResolver
import android.net.InetAddresses
import android.os.CancellationSignal
import kotlinx.coroutines.suspendCancellableCoroutine
import java.net.InetAddress
import java.util.concurrent.Executor
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

fun interface SSHResolver { suspend fun resolve(host: String): List<InetAddress> }
/** API 29+ cancellable DNS; unlike blocking getAllByName, cancellation reaches Android. */
object AndroidSSHResolver : SSHResolver {
    private val callbacks = Executor { it.run() }
    override suspend fun resolve(host: String): List<InetAddress> {
        if (InetAddresses.isNumericAddress(host)) return listOf(InetAddresses.parseNumericAddress(host))
        return suspendCancellableCoroutine { waiting ->
            val cancellation = CancellationSignal()
            waiting.invokeOnCancellation { cancellation.cancel() }
            DnsResolver.getInstance().query(null,host,DnsResolver.FLAG_EMPTY,callbacks,cancellation,
                object : DnsResolver.Callback<List<InetAddress>> {
                    override fun onAnswer(answer: List<InetAddress>, rcode: Int) {
                        if (waiting.isActive) {
                            if (rcode == 0 && answer.isNotEmpty()) waiting.resume(answer)
                            else waiting.resumeWithException(IllegalStateException("无法解析主机地址（DNS $rcode）"))
                        }
                    }
                    override fun onError(error: DnsResolver.DnsException) {
                        if (waiting.isActive) waiting.resumeWithException(IllegalStateException("主机地址解析失败，请检查网络"))
                    }
                })
        }
    }
}
