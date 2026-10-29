package com.hanxi.ghosterm.bridge

/** Control-plane bridge. Never carries terminal output. */
object CoreLibrary {
    init { System.loadLibrary("ghosterm_android") }
    val version: String get() = versionBytes().toString(Charsets.UTF_8)
    fun roundTrip(configuration: ByteArray): ByteArray = roundTripBytes(configuration)
    private external fun versionBytes(): ByteArray
    private external fun roundTripBytes(configuration: ByteArray): ByteArray
}
