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
            "\"notification\":{\"method\":\"item/agentMessage/delta\",\"params\":{\"threadId\":\"thread\",\"turnId\":\"turn\",\"itemId\":\"m1\",\"delta\":\"hello\"}}"))
        state = CodexHostEvents.accept(state, first)
        val second = CodexHostEvents.decode(rpc(2, "notification",
            "\"notification\":{\"method\":\"item/agentMessage/delta\",\"params\":{\"threadId\":\"thread\",\"turnId\":\"turn\",\"itemId\":\"m1\",\"delta\":\" world\"}}"))
        state = CodexHostEvents.accept(state, second)
        assertEquals(2, state.sequence)
        assertEquals(1, state.timeline.size)
        assertEquals("hello world", state.timeline.single().detail)
        val gap = CodexHostEvents.decode(rpc(4, "closed", "\"outcome\":\"clean\""))
        assertThrows(IllegalArgumentException::class.java) { CodexHostEvents.accept(state, gap) }
    }

    @Test fun approvalsAreBoundedAndResolvedByTypedRequestID() {
        val event = CodexHostEvents.decode(rpc(1, "approval-request",
            "\"request\":{\"id\":42,\"method\":\"item/fileChange/requestApproval\",\"params\":{\"threadId\":\"thread\",\"turnId\":\"turn\",\"itemId\":\"change-1\"}}"))
        var state = CodexHostEvents.accept(CodexHostState(), event)
        assertEquals("n:42", state.approvals.single().id)
        assertEquals("change-1", state.approvals.single().itemID)
        assertEquals(CodexApprovalIdentity("thread", "turn", "change-1"), state.approvals.single().identity)
        val duplicate = CodexHostEvents.decode(rpc(2, "approval-request",
            "\"request\":{\"id\":42,\"method\":\"item/fileChange/requestApproval\",\"params\":{\"threadId\":\"thread\",\"turnId\":\"turn\",\"itemId\":\"change-1\"}}"))
        assertThrows(IllegalArgumentException::class.java) { CodexHostEvents.accept(state, duplicate) }
        val resolved = CodexHostEvents.decode(rpc(2, "approval-resolved", "\"requestID\":42"))
        state = CodexHostEvents.accept(state, resolved)
        assertTrue(state.approvals.isEmpty())
    }

    @Test fun approvalRejectsBooleanIDsAndUnknownMethods() {
        assertThrows(IllegalArgumentException::class.java) {
            CodexHostEvents.decode(rpc(1, "approval-request",
                "\"request\":{\"id\":1,\"method\":\"item/fileChange/requestApproval\",\"params\":{\"itemId\":\"missing-thread-turn\"}}"))
        }
        assertThrows(IllegalArgumentException::class.java) {
            CodexHostEvents.decode(rpc(1, "approval-request",
                "\"request\":{\"id\":true,\"method\":\"item/commandExecution/requestApproval\",\"params\":{}}"))
        }
        assertThrows(IllegalArgumentException::class.java) {
            CodexHostEvents.decode(rpc(1, "approval-request",
                "\"request\":{\"id\":\"req-1\",\"method\":\"other/requestApproval\",\"params\":{}}"))
        }
    }

    @Test fun turnCompletionAndUncertainTerminalEventsExpirePendingApprovals() {
        for (terminal in listOf("notification", "protocol-error", "closed", "outcome-unknown")) {
            val pending = CodexHostEvents.decode(rpc(1, "approval-request",
                "\"request\":{\"id\":42,\"method\":\"item/fileChange/requestApproval\",\"params\":{\"threadId\":\"thread\",\"turnId\":\"turn\",\"itemId\":\"change\"}}"))
            val state = CodexHostEvents.accept(CodexHostState(turnActive = true), pending)
            val payload = if (terminal == "notification")
                "\"notification\":{\"method\":\"turn/completed\",\"params\":{\"threadId\":\"thread\",\"turn\":{\"id\":\"turn\",\"status\":\"interrupted\"}}}"
            else "\"outcome\":\"unknown\""
            val expired = CodexHostEvents.accept(state, CodexHostEvents.decode(rpc(2, terminal, payload)))
            assertTrue(terminal, expired.approvals.isEmpty())
            assertEquals(terminal != "notification", expired.turnActive)
            assertEquals(2, expired.rawEvents.size)
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

    @Test fun completeSnapshotReferenceSurvivesAnEntirelyOmittedPreview() {
        val marker = JSONObject().put("snapshotID", "change-7").put("snapshotUnits", 50)
            .put("snapshotBytes", 60).put("omittedChanges", 10)
        val params = JSONObject().put("threadId", "thread").put("turnId", "turn").put("itemId", "item")
            .put("changes", org.json.JSONArray()).put("_ghosterm", marker)
        val notification = JSONObject().put("method", "item/fileChange/patchUpdated").put("params", params)
        val event = CodexHostEvents.decode(rpc(1, "notification", "\"notification\":$notification"))
        val state = CodexHostEvents.accept(CodexHostState(), event)
        val item = state.timeline.single()
        assertTrue(item.fileChanges.isEmpty())
        assertEquals(10, item.omittedChangeCount)
        assertEquals("change-7", item.changeReference?.snapshotID)
        assertEquals("thread", item.changeReference?.threadID)
        val completed = JSONObject().put("method", "item/completed").put("params", JSONObject()
            .put("threadId", "thread").put("turnId", "turn").put("item", JSONObject().put("id", "item")
                .put("type", "fileChange").put("changes", org.json.JSONArray()).put("_ghosterm", marker)))
        val next = CodexHostEvents.accept(state, CodexHostEvents.decode(rpc(2, "notification", "\"notification\":$completed")))
        assertEquals(item.changeReference, next.timeline.single().changeReference)
    }
}
