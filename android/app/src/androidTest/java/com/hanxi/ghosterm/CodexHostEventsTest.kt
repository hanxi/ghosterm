package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionSession
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class CodexHostEventsTest {
    private fun rpc(sequence: Int, kind: String, payload: String): ExtensionSession.RPCRequest {
        val event = "{\"sequence\":$sequence,\"kind\":\"$kind\",$payload}"
        return ExtensionSession.RPCRequest(1, "event-$sequence", "codex.event",
            JSONObject().put("event", event).toString().toByteArray(Charsets.UTF_8))
    }

    @Test fun notificationSequenceMergesMessageDeltasAndRejectsGaps() {
        var state = CodexHostState()
        val first = CodexHostEvents.decode(rpc(1, "notification",
            "\"notification\":{\"method\":\"item/agentMessage/delta\",\"params\":{\"itemId\":\"m1\",\"delta\":\"hello\"}}"))
        state = CodexHostEvents.accept(state, first)
        val second = CodexHostEvents.decode(rpc(2, "notification",
            "\"notification\":{\"method\":\"item/agentMessage/delta\",\"params\":{\"itemId\":\"m1\",\"delta\":\" world\"}}"))
        state = CodexHostEvents.accept(state, second)
        assertEquals(2, state.sequence)
        assertEquals(1, state.timeline.size)
        assertEquals("hello world", state.timeline.single().detail)
        val gap = CodexHostEvents.decode(rpc(4, "closed", "\"outcome\":\"clean\""))
        assertThrows(IllegalArgumentException::class.java) { CodexHostEvents.accept(state, gap) }
    }

    @Test fun approvalsAreBoundedAndResolvedByTypedRequestID() {
        val event = CodexHostEvents.decode(rpc(1, "approval-request",
            "\"request\":{\"id\":42,\"method\":\"item/fileChange/requestApproval\",\"params\":{\"itemId\":\"change-1\"}}"))
        var state = CodexHostEvents.accept(CodexHostState(), event)
        assertEquals("n:42", state.approvals.single().id)
        assertEquals("change-1", state.approvals.single().itemID)
        val duplicate = CodexHostEvents.decode(rpc(2, "approval-request",
            "\"request\":{\"id\":42,\"method\":\"item/fileChange/requestApproval\",\"params\":{}}"))
        assertThrows(IllegalArgumentException::class.java) { CodexHostEvents.accept(state, duplicate) }
        val resolved = CodexHostEvents.decode(rpc(2, "approval-resolved", "\"requestID\":42"))
        state = CodexHostEvents.accept(state, resolved)
        assertTrue(state.approvals.isEmpty())
    }

    @Test fun approvalRejectsBooleanIDsAndUnknownMethods() {
        assertThrows(IllegalArgumentException::class.java) {
            CodexHostEvents.decode(rpc(1, "approval-request",
                "\"request\":{\"id\":true,\"method\":\"item/commandExecution/requestApproval\",\"params\":{}}"))
        }
        assertThrows(IllegalArgumentException::class.java) {
            CodexHostEvents.decode(rpc(1, "approval-request",
                "\"request\":{\"id\":\"req-1\",\"method\":\"other/requestApproval\",\"params\":{}}"))
        }
    }

    @Test fun filePatchUpdatesProduceBoundedTypedDiffEntries() {
        val event = CodexHostEvents.decode(rpc(1, "notification",
            "\"notification\":{\"method\":\"item/fileChange/patchUpdated\",\"params\":{" +
                "\"itemId\":\"change-1\",\"changes\":[{\"path\":\"src/A.kt\",\"kind\":{\"type\":\"update\",\"move_path\":null}," +
                "\"diff\":\"@@ -1 +1 @@\\n-old\\n+new\\n…（diff 已截断）\"}]," +
                "\"_ghosterm\":{\"truncated\":true,\"omittedChanges\":2,\"diffTruncated\":true}}}"))
        val state = CodexHostEvents.accept(CodexHostState(), event)
        val item = state.timeline.single()
        assertEquals("文件变更", item.title)
        assertEquals(2, item.omittedChangeCount)
        assertEquals("src/A.kt", item.fileChanges.single().path)
        assertEquals(CodexDiffLine.Kind.HUNK, item.fileChanges.single().lines.first().kind)
        assertTrue(item.fileChanges.single().diffTruncated)
    }
}
