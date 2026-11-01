package com.hanxi.ghosterm

import android.net.DnsResolver
import android.net.InetAddresses
import android.os.CancellationSignal
import android.system.ErrnoException
import android.system.OsConstants
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.suspendCancellableCoroutine
import java.io.IOException
import android.icu.text.IDNA
import java.net.InetAddress
import java.util.concurrent.Executor
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

fun interface SSHResolver { suspend fun resolve(host: String): List<InetAddress> }

// Linux UAPI errno for Android's shipped arm64 ABI (NDK asm-generic/errno.h).
// The OsConstants field is public only from API 31; ErrnoException uses 64 on 29 too.
private const val ANDROID_ENONET = 64

enum class SSHResolveFailure { INVALID_HOST, NAME_NOT_FOUND, NO_ADDRESS, DNS_FAILURE, RESPONSE_INVALID, NETWORK_UNAVAILABLE, TOO_MANY_ADDRESSES }
class SSHResolveException(val failure: SSHResolveFailure, message: String,
    val responseCode: Int? = null, val resolverCode: Int? = null, val systemError: Int? = null,
    cause: Throwable? = null) : IOException(message,cause)

/** The platform seam keeps real Android cancellation and typed callbacks testable. */
internal data class SSHDnsError(val code: Int, val cause: Throwable?, val original: Throwable)
internal interface SSHDnsCallback {
    fun onAnswer(answer: List<InetAddress>, rcode: Int)
    fun onError(error: SSHDnsError)
}
internal fun interface SSHDnsQuery {
    fun query(host: String, cancellation: CancellationSignal, callback: SSHDnsCallback)
}
internal object PlatformSSHDnsQuery : SSHDnsQuery {
    private val callbacks=Executor { it.run() }
    override fun query(host: String, cancellation: CancellationSignal, callback: SSHDnsCallback) {
        DnsResolver.getInstance().query(null,host,DnsResolver.FLAG_EMPTY,callbacks,cancellation,
            object: DnsResolver.Callback<List<InetAddress>> {
                override fun onAnswer(answer: List<InetAddress>, rcode: Int)=callback.onAnswer(answer,rcode)
                override fun onError(error: DnsResolver.DnsException)=callback.onError(SSHDnsError(error.code,error.cause,error))
            })
    }
}
/** API 29+ system DNS only: no public resolver fallback or change of SSH host identity. */
object AndroidSSHResolver : SSHResolver by SystemSSHResolver()

internal class SystemSSHResolver(private val dns: SSHDnsQuery = PlatformSSHDnsQuery) : SSHResolver {
    private val idna by lazy { IDNA.getUTS46Instance(IDNA.CHECK_BIDI or IDNA.CHECK_CONTEXTJ or IDNA.NONTRANSITIONAL_TO_ASCII) }
    private fun dnsName(host: String): String {
        // Preserve existing ASCII DNS forms (including underscores and root dot).
        val ascii=if(host.all { it.code<128 })host else {
            val info=IDNA.Info();val output=StringBuilder()
            idna.nameToASCII(host,output,info);require(!info.hasErrors())
            output.toString()
        }
        val plain=ascii.removeSuffix(".")
        require(plain.isNotEmpty() && plain.length<=253 && ascii.length<=254 && plain.split('.').all { it.isNotEmpty() && it.length<=63 })
        return ascii
    }
    override suspend fun resolve(host: String): List<InetAddress> {
        if(InetAddresses.isNumericAddress(host))return listOf(InetAddresses.parseNumericAddress(host))
        val name=try {
            require(host.isNotEmpty() && host.none { it.isWhitespace() || it.isISOControl() })
            dnsName(host)
        } catch(error:IllegalArgumentException) {
            throw SSHResolveException(SSHResolveFailure.INVALID_HOST,"主机地址格式无效，请检查域名或 IP 地址",cause=error)
        }
        return suspendCancellableCoroutine { waiting ->
            val cancellation=CancellationSignal()
            waiting.invokeOnCancellation { cancellation.cancel() }
            val callback=object : SSHDnsCallback {
                override fun onAnswer(answer: List<InetAddress>, rcode: Int) {
                    if(!waiting.isActive)return
                    val failure=when {
                        rcode==3 -> SSHResolveException(SSHResolveFailure.NAME_NOT_FOUND,"DNS 未找到此域名，请检查主机地址",responseCode=rcode)
                        rcode!=0 -> SSHResolveException(SSHResolveFailure.DNS_FAILURE,"DNS 查询未成功（响应码 $rcode），请检查 DNS 或稍后重试",responseCode=rcode)
                        answer.isEmpty() -> SSHResolveException(SSHResolveFailure.NO_ADDRESS,"DNS 未返回可用的主机地址",responseCode=rcode)
                        answer.size>32 -> SSHResolveException(SSHResolveFailure.TOO_MANY_ADDRESSES,"主机地址数量超过连接上限（32）",responseCode=rcode)
                        else -> null
                    }
                    if(failure!=null)waiting.resumeWithException(failure) else waiting.resume(answer.toList())
                }
                override fun onError(error: SSHDnsError) {
                    if(!waiting.isActive)return
                    val errno=(error.cause as? ErrnoException)?.errno
                    val unavailable=error.code==DnsResolver.ERROR_SYSTEM && errno in setOf(ANDROID_ENONET,OsConstants.ENETDOWN,OsConstants.ENETUNREACH)
                    val kind=when {
                        error.code==DnsResolver.ERROR_PARSE -> SSHResolveFailure.RESPONSE_INVALID
                        unavailable -> SSHResolveFailure.NETWORK_UNAVAILABLE
                        else -> SSHResolveFailure.DNS_FAILURE
                    }
                    val message=when {
                        kind==SSHResolveFailure.RESPONSE_INVALID -> "DNS 应答无法读取，请检查 DNS 服务"
                        unavailable -> "当前网络不可用，无法解析主机地址"
                        error.code==DnsResolver.ERROR_SYSTEM && errno==OsConstants.ETIMEDOUT -> "主机地址解析超时，请检查 DNS 或网络"
                        else -> "主机地址解析失败，请检查 DNS 或网络"
                    }
                    waiting.resumeWithException(SSHResolveException(kind,message,resolverCode=error.code,systemError=errno,cause=error.original))
                }
            }
            try { dns.query(name,cancellation,callback) }
            catch(cancel:CancellationException) { waiting.cancel(cancel) }
            catch(error:Exception) {
                if(waiting.isActive)waiting.resumeWithException(SSHResolveException(SSHResolveFailure.DNS_FAILURE,"无法开始主机地址解析，请检查 DNS 或网络",cause=error))
            }
        }
    }
}
