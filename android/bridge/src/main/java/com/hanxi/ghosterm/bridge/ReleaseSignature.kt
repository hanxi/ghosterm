package com.hanxi.ghosterm.bridge

import android.os.Looper

/** Crypto-only Core operation. No policy, installation or runtime authorization. */
object ReleaseSignature {
    /** Core status: 0 valid, 1 invalid arguments, 4 mismatch, 7 unsupported, 9 incomplete. */
    fun verify(publicKey: ByteArray, signature: ByteArray, descriptor: ByteArray): Int {
        check(Looper.myLooper() != Looper.getMainLooper()) { "发布签名须在非 UI worker 验证" }
        if (publicKey.size != 32 || signature.size != 64 || descriptor.size !in 1..65536) return 1
        return NativeReleaseSignature.verify(publicKey, signature, descriptor)
    }
}
internal object NativeReleaseSignature {
    init { System.loadLibrary("ghosterm_android") }
    external fun verify(publicKey: ByteArray, signature: ByteArray, descriptor: ByteArray): Int
}
