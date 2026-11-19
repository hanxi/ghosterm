package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

internal class PluginViewsFixture(val instance: String = "b".repeat(32)) {
    val nonce = "a".repeat(32)
    val alpha = PluginTableFixture(); val beta = PluginTableFixture().apply { description.put("viewId", "table-b") }
    val diff = PluginDiffFixture(); val task = PluginTaskFixture()
    var revision = 1uL; var current = true; var closeFails = false
    var pauseClose = false
    val closeEntered = CompletableDeferred<Unit>(); val closeRelease = CompletableDeferred<Unit>()
    val closes = mutableListOf<String>()
    fun entry(n: Int, id: String, kind: String, prefix: String) = JSONObject().put("handle", "$nonce.$n").put("viewId", id).put("kind", kind)
        .put("methods", JSONObject().put("describe", "$prefix.describe").put("read", "$prefix.read").apply { if (kind == "task") put("cancel", "$prefix.cancel") })
    val entries = mutableListOf(entry(1, "table-a", "table", "alpha"), entry(2, "table-b", "table", "beta"),
        entry(3, "diff-a", "diff", "changes"), entry(4, "task-a", "task", "work"))
    fun data(instance: String = this.instance, epoch: String = "9007199254740993", revision: String = this.revision.toString()) = JSONObject()
        .put("instanceId", instance).put("epoch", epoch).put("revision", revision).put("views", JSONArray(entries)).toString().toByteArray()
    val methods get() = entries.flatMap { entry -> val m = entry.getJSONObject("methods"); m.keys().asSequence().map { m.getString(it) }.toList() }.toSet()
    fun model(scope: CoroutineScope, kinds: Set<PluginViewRegistration.Kind> = PluginViewRegistration.Kind.entries.toSet()) =
        PluginViewsModel(scope, instance, "9007199254740993", kinds, setOf("session.rpc", "ui.views", "ui.table", "ui.diff", "ui.task"), methods,
            request = { name, input ->
                val prefix = name.substringBefore('.'); val suffix = name.substringAfter('.')
                when (prefix) {
                    "alpha" -> alpha.request(if (suffix == "describe") "view.table.describe" else "view.table.page", input)
                    "beta" -> beta.request(if (suffix == "describe") "view.table.describe" else "view.table.page", input)
                    "changes" -> diff.request(if (suffix == "describe") "view.diff.describe" else "view.diff.page", input)
                    else -> task.request("view.task.$suffix", input)
                }
            }, isCurrent = { current }, close = { handle ->
                closes += handle
                if (pauseClose) { closeEntered.complete(Unit); closeRelease.await() }
                check(!closeFails) { "Fixture unconfirmed close" }
                entries.removeAll { it.getString("handle") == handle }; revision++; data()
            })
    fun populated(scope: CoroutineScope) = model(scope).also { it.apply(data()) }
}

@RunWith(AndroidJUnit4::class)
class PluginViewsTest {
    private fun main(block: suspend CoroutineScope.() -> Unit) = runBlocking { withContext(Dispatchers.Main.immediate) { block() } }
    private fun rejects(block: () -> Unit): Boolean = try { block(); false } catch (_: Exception) { true }
    private fun table(m: PluginViewsModel, index: Int = 0) = (m.state.value.items[index].content as PluginViewItem.Content.Table).model
    private fun task(m: PluginViewsModel) = (m.state.value.items.last().content as PluginViewItem.Content.Task).model
    @Test fun fourIndependentModelsKeepTheirOwnPagesAndIdentity() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); val old = m.state.value.items
        m.apply(f.data()); assertTrue(old.zip(m.state.value.items).all { it.first === it.second })
        table(m).load()!!.join(); table(m, 1).load()!!.join()
        (m.state.value.items[2].content as PluginViewItem.Content.Diff).model.load()!!.join(); task(m).load()!!.join()
        table(m).showPage(16)!!.join()
        assertTrue(table(m).state.value.page?.offset == 16 && table(m, 1).state.value.page?.offset == 0 && task(m).state.value.snapshot != null)
    }
    @Test fun overlappingHandlesRemainOwnedByTheirOwnInstances() = main {
        val first = PluginViewsFixture()
        val second = PluginViewsFixture(instance = "c".repeat(32))
        val firstModel = first.populated(this)
        val secondModel = second.populated(this)
        val firstItem = firstModel.state.value.items.first()
        val secondItem = secondModel.state.value.items.first()
        assertEquals(firstItem.handle, secondItem.handle)
        val firstTable = table(firstModel)
        val secondTable = table(secondModel)
        firstTable.load()!!.join()
        secondTable.load()!!.join()

        first.pauseClose = true
        val closing = firstModel.closeView(firstItem.handle)!!
        try {
            withTimeout(5000) { first.closeEntered.await() }
            secondTable.showPage(16)!!.join()
            assertEquals("17", secondTable.state.value.page?.rows?.firstOrNull()?.id)
            assertTrue(secondTable.available)
        } finally {
            first.closeRelease.complete(Unit)
            closing.join()
        }

        assertFalse(firstTable.available)
        assertNull(firstTable.state.value.page)
        assertTrue(secondModel.state.value.items.first() === secondItem)
        assertEquals(listOf(firstItem.handle), first.closes)
        assertTrue(second.closes.isEmpty())
        secondTable.showPage(0)!!.join()
        assertEquals("1", secondTable.state.value.page?.rows?.firstOrNull()?.id)
    }

    @Test fun lateCloseAfterRuntimeInvalidationDoesNotAffectAnotherInstance() = main {
        val first = PluginViewsFixture()
        val second = PluginViewsFixture(instance = "c".repeat(32))
        val firstModel = first.populated(this)
        val secondModel = second.populated(this)
        val handle = firstModel.state.value.items.first().handle
        val survivingItem = secondModel.state.value.items.first()
        assertEquals(handle, survivingItem.handle)
        val survivingTable = table(secondModel)
        survivingTable.load()!!.join()

        first.pauseClose = true
        val closing = firstModel.closeView(handle)!!
        try {
            withTimeout(5000) { first.closeEntered.await() }
            firstModel.invalidate()
            survivingTable.showPage(16)!!.join()
        } finally {
            first.closeRelease.complete(Unit)
            closing.join()
        }

        assertFalse(firstModel.state.value.valid)
        assertTrue(firstModel.state.value.items.isEmpty())
        assertNull(firstModel.state.value.issue)
        assertEquals(listOf(handle), first.closes)
        assertTrue(second.closes.isEmpty())
        assertSame(survivingItem, secondModel.state.value.items.first())
        assertEquals("17", survivingTable.state.value.page?.rows?.firstOrNull()?.id)
    }

    @Test fun wrongInstanceClearsViews() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); f.revision++
        assertTrue(rejects { m.apply(f.data(instance = "c".repeat(32))) } && !m.state.value.valid && m.state.value.items.isEmpty())
    }
    @Test fun wrongEpochClearsViews() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); f.revision++
        assertTrue(rejects { m.apply(f.data(epoch = "9007199254740992")) } && m.state.value.items.isEmpty())
    }
    @Test fun sameRevisionCannotChangeBindings() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); f.entries.removeAt(3)
        assertTrue(rejects { m.apply(f.data()) } && !m.state.value.valid)
    }
    @Test fun olderSnapshotCannotResurrectClosedViews() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); val old = f.data()
        m.closeView(m.state.value.items[0].handle)!!.join(); m.apply(old)
        assertTrue(m.state.value.valid && m.state.value.items.size == 3)
    }
    @Test fun removedHandleCannotReturnAtNewRevision() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); val removed = f.entries[0]
        m.closeView(m.state.value.items[0].handle)!!.join(); f.entries += removed; f.revision++
        assertTrue(rejects { m.apply(f.data()) } && !m.state.value.valid)
    }
    @Test fun bindingsCannotChangeUnderExistingHandle() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); f.entries[0].getJSONObject("methods").put("describe", "beta.describe"); f.revision++
        assertTrue(rejects { m.apply(f.data()) })
    }
    @Test fun reopenSameViewIDUsesFreshModel() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); val old = table(m)
        old.load()!!.join(); old.showPage(16)!!.join(); m.closeView(m.state.value.items[0].handle)!!.join()
        f.entries += f.entry(5, "table-a", "table", "alpha"); f.revision++; m.apply(f.data())
        val reopened = table(m, 3); reopened.load()!!.join()
        assertTrue(!old.state.value.valid && reopened !== old && reopened.state.value.page?.offset == 0)
    }
    @Test fun unknownMethodBindingRejected() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); f.entries += f.entry(5, "new-table", "table", "unapproved"); f.revision++
        assertTrue(rejects { m.apply(f.data()) } && !m.state.value.valid)
        val issue = checkNotNull(m.state.value.issue)
        m.invalidate()
        assertEquals(issue, m.state.value.issue)
        assertTrue(m.state.value.items.isEmpty())
    }
    @Test fun undeclaredKindRejected() = main {
        val f = PluginViewsFixture(); val m = f.model(this, setOf(PluginViewRegistration.Kind.TABLE))
        assertTrue(rejects { m.apply(f.data()) })
    }
    @Test fun snapshotBoundsDuplicatesAndCanonicalCounters() {
        val f = PluginViewsFixture()
        for (revision in listOf("0", "01", "18446744073709551616")) assertTrue(rejects { PluginViewRegistrySnapshot.parse(f.data(revision = revision)) })
        f.entries += f.entries[0]; assertTrue(rejects { PluginViewRegistrySnapshot.parse(f.data()) })
        assertTrue(rejects { PluginViewRegistrySnapshot.parse(ByteArray(8193) { 32 }) })
        f.entries.clear(); repeat(9) { f.entries += f.entry(it + 1, "t$it", "table", "alpha") }
        assertTrue(rejects { PluginViewRegistrySnapshot.parse(f.data()) })
    }
    @Test fun duplicateRawJSONKeysRejected() {
        val raw = PluginViewsFixture().data().toString(Charsets.UTF_8)
        assertTrue(rejects { PluginViewRegistrySnapshot.parse(("{\"revision\":\"1\"," + raw.drop(1)).toByteArray()) })
    }
    @Test fun descriptorsMustMatchRegisteredViewID() = main {
        val f = PluginViewsFixture(); val m = f.populated(this)
        f.alpha.description.put("viewId", "other"); f.diff.description.put("viewId", "other"); f.task.description.put("viewId", "other")
        val a = table(m); val d = (m.state.value.items[2].content as PluginViewItem.Content.Diff).model; val t = task(m)
        a.load()!!.join(); d.load()!!.join(); t.load()!!.join()
        assertTrue(a.state.value.descriptor == null && a.state.value.issue != null && d.state.value.descriptor == null && d.state.value.issue != null &&
            t.state.value.descriptor == null && t.state.value.issue != null)
    }
    @Test fun nativeCloseDoesNotCancelTask() = main {
        val f = PluginViewsFixture(); val m = f.populated(this)
        m.closeView(m.state.value.items.last().handle)!!.join()
        assertTrue(f.closes.size == 1 && f.task.calls.isEmpty() && m.state.value.items.size == 3)
    }
    @Test fun failedCloseRetainsViewAndAllowsExplicitRetry() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); val item = m.state.value.items[0]; f.closeFails = true
        m.closeView(item.handle)!!.join(); assertTrue(m.state.value.items.size == 4 && !item.closing && item.closeIssue != null)
        f.closeFails = false; m.closeView(item.handle)!!.join(); assertEquals(3, m.state.value.items.size)
    }
    @Test fun invalidationRejectsLatePage() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); val t = table(m); f.alpha.pause = "view.table.page"
        val pending = t.load()!!; withTimeout(5000) { f.alpha.entered.await() }
        m.invalidate(); f.alpha.release.complete(Unit); pending.join()
        assertTrue(t.state.value.page == null && !t.state.value.valid && m.state.value.items.isEmpty())
    }
    @Test fun invalidationRejectsLateCloseSnapshot() = main {
        val f = PluginViewsFixture(); val m = f.populated(this)
        f.pauseClose = true
        val item = m.state.value.items.first()
        val pending = checkNotNull(m.closeView(item.handle))
        withTimeout(5000) { f.closeEntered.await() }
        m.invalidate()
        f.closeRelease.complete(Unit)
        pending.join()
        assertFalse(m.state.value.valid)
        assertTrue(m.state.value.items.isEmpty())
        assertNull(item.closeIssue)
        assertNull(m.closeView(item.handle))
        assertEquals(listOf(item.handle), f.closes)
    }
    @Test fun taskReopenKeepsUnknownCancellationIdentity() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); val old = task(m)
        old.load()!!.join(); f.task.unknownMethod = "view.task.cancel"; old.cancel()!!.join(); val id = old.state.value.cancelRequestID
        m.closeView(m.state.value.items.last().handle)!!.join(); f.entries += f.entry(5, "task-a", "task", "work"); f.revision++; m.apply(f.data())
        val new = task(m); new.load()!!.join(); new.cancel()?.join()
        assertTrue(new.state.value.cancelRequestID == id && new.state.value.cancellation == PluginTaskModel.Cancellation.UNCONFIRMED && f.task.calls.count { it.first == "view.task.cancel" } == 1)
    }
    @Test fun taskReopenCannotRewriteConfirmedTerminal() = main {
        val f = PluginViewsFixture(); val m = f.populated(this); f.task.finish("succeeded"); task(m).load()!!.join()
        m.closeView(m.state.value.items.last().handle)!!.join(); f.entries += f.entry(5, "task-a", "task", "work"); f.revision++; m.apply(f.data())
        f.task.state.put("sequence", "3").put("phase", "running").put("result", JSONObject().put("kind", "none"))
        task(m).load()!!.join(); assertTrue(task(m).state.value.snapshot?.phase == PluginTaskSnapshot.Phase.SUCCEEDED && task(m).state.value.issue != null)
    }
    @Test fun taskHistoryDoesNotEvictAtCapacity() = main {
        val h = PluginTaskHistory(); val f = PluginTaskFixture()
        repeat(9) { i ->
            f.description.put("viewId", "task-$i"); f.state.put("viewId", "task-$i")
            val t = PluginTaskModel(this, f::request, history = h); t.load()!!.join()
            if (i == 8) assertTrue(t.state.value.descriptor == null && t.state.value.issue != null)
        }
        assertEquals(8, h.retained)
    }
    @Test fun bootstrapAcceptsFourUniqueImplementedServices() {
        val hash = "a".repeat(64); val services = listOf("ui.table", "ui.diff", "ui.task", "ui.views")
        val r = ExtensionRelayBootstrap.Request("/gterm", "/package", hash, hash, listOf("alpha.page"), command = "views demo", clientCapabilities = services)
        val command = ExtensionRelayBootstrap.build(r)
        for (service in services) assertTrue(command.contains("'--client-capability' '$service'"))
        for (invalid in listOf(services + "ui.views", listOf("ui.views", "ui.views"), listOf("ui.missing"))) {
            assertTrue(rejects { ExtensionRelayBootstrap.build(r.copy(clientCapabilities = invalid)) })
        }
    }
}
