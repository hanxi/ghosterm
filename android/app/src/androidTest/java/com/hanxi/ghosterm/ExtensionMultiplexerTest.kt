package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionChannel
import com.hanxi.ghosterm.bridge.ExtensionMultiplexer
import com.hanxi.ghosterm.bridge.ExtensionSession
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ExtensionMultiplexerTest {
    @Test fun eightSessionsShareOneConnectionAndCloseIndependently() {
        val descriptors = ParcelFileDescriptor.createSocketPair()
        val channels = descriptors.map { val fd = it.detachFd(); ExtensionChannel.adoptDescriptors(fd, fd) }
        val connection = "a".repeat(32)
        val muxes = channels.map { ExtensionMultiplexer.adopt(it, connection) }
        assertThrows(IllegalStateException::class.java) { channels[0].info() }
        val sessions = List(2) { mutableListOf<ExtensionSession>() }
        fun identity(slot: Int) = (slot + 1).toString(16).padStart(32, '0')
        fun pump() {
            sessions.indices.forEach { side -> sessions[side].forEach { it.step() }; assertEquals(0, muxes[side].step()) }
        }
        try {
            val config = ExtensionSession.Config("org.ghosterm.mux", "0.1.0", "b".repeat(64), "c".repeat(64), "0.1.0",
                required = listOf("session.rpc"), available = listOf("session.rpc"),
                peerKind = ExtensionSession.PeerKind.LOCAL, peerIdentity = "owned-mux", methods = listOf(
                    ExtensionSession.Method("fixture.echo", "session.rpc", ExtensionSession.Role.EXECUTOR,
                        ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT)))
            repeat(2) { side -> repeat(8) { slot ->
                sessions[side] += ExtensionSession.adopt(muxes[side].openChannel(identity(slot), (slot + 1).toULong()),
                    if (side == 0) ExtensionSession.Role.HOST else ExtensionSession.Role.EXECUTOR, config)
            }; assertEquals(8, muxes[side].info().instances) }
            for (step in 0 until 2000) {
                pump()
                sessions[0].forEach { session -> val state = session.info()
                    if (state.phase == ExtensionSession.Phase.CONSENT) assertEquals(0, session.decide(state.consentToken, ExtensionSession.Decision.ALLOW)) }
                if (sessions.flatten().all { it.info().phase == ExtensionSession.Phase.ACTIVE }) break
            }
            sessions.flatten().forEach { assertEquals(ExtensionSession.Phase.ACTIVE, it.info().phase); assertEquals(connection, it.info().connection) }
            val leases = sessions.map { side -> side.map { it.lease()!! } }
            val handles = (0 until 8).map { slot ->
                val admission = sessions[0][slot].request(leases[0][slot], "fixture.echo", "{\"slot\":$slot}".toByteArray())
                assertEquals(ExtensionSession.RPCStatus.OK, admission.status); admission.handle!!
            }
            val completed = mutableSetOf<Int>()
            for (step in 0 until 2000) {
                pump()
                repeat(8) { slot ->
                    sessions[1][slot].takeRequest(leases[1][slot]).second?.let { request ->
                        assertArrayEquals("{\"slot\":$slot}".toByteArray(), request.input)
                        assertEquals(ExtensionSession.RPCStatus.OK, sessions[1][slot].respond(leases[1][slot], request.ticket,
                            "{\"kind\":\"success\",\"value\":{\"slot\":$slot}}".toByteArray()))
                    }
                    sessions[0][slot].requestResult(leases[0][slot], handles[slot]).second?.let { result ->
                        if (result.confirmed) { assertEquals(ExtensionSession.RPCState.SUCCEEDED, result.state); completed += slot }
                    }
                }
                if (completed.size == 8) break
            }
            assertEquals(8, completed.size); assertTrue(muxes[0].transportInfo().sent > 0)
            for (step in 0 until 1000) { if (sessions[0][3].closeStep()) break; pump() }
            sessions[0].removeAt(3)
            repeat(50) { pump() }
            assertEquals(ExtensionSession.End.PEER, sessions[1][3].info().end)
            assertTrue(sessions[0].all { it.info().phase == ExtensionSession.Phase.ACTIVE })
            val final = muxes[0].scopeInfo(identity(3), 4uL)
            assertTrue(final.released && final.closeSent)
            assertThrows(Exception::class.java) { muxes[0].scopeInfo(identity(3), 99uL) }
            assertFalse(muxes[0].closeStep()); assertEquals(7, muxes[0].info().instances)
        } finally {
            muxes.forEach { it.stop() }
            sessions.flatten().forEach { it.close() }
            muxes.forEach { it.close() }
        }
    }
}
