package com.hanxi.ghosterm

import android.app.Application
import android.app.Instrumentation
import android.os.Bundle
import android.os.Process
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import java.io.File
import java.io.FileOutputStream

/** An explicitly selected host probe. With no journalCrash argument it contributes
 * zero tests, not a passing no-op or a skipped crash check, to ordinary JUnit suites. */
@org.junit.runner.RunWith(FileJournalProbeRunner::class)
class FileJournalCrashProbe

class FileJournalProbeRunner(@Suppress("UNUSED_PARAMETER") type: Class<*>): org.junit.runner.Runner(),
    org.junit.runner.manipulation.Filterable {
    private val arguments=androidx.test.platform.app.InstrumentationRegistry.getArguments()
    private val instrumentation=androidx.test.platform.app.InstrumentationRegistry.getInstrumentation()
    private val targetContext get()=instrumentation.targetContext
    private val enabled=arguments.getString("journalCrash")=="true"
    private val description=if(enabled) org.junit.runner.Description.createTestDescription(FileJournalCrashProbe::class.java,
        "${arguments.getString("action")}-${arguments.getString("point")}")
        else org.junit.runner.Description.createSuiteDescription("Host-only file journal crash probes")
    override fun getDescription()=description
    override fun filter(filter:org.junit.runner.manipulation.Filter) {
        // JUnit counts a childless suite as a test. Remove the disabled runner
        // from the parent suite, including Android's log-only discovery wrapper.
        if(!enabled || !filter.shouldRun(description))
            throw org.junit.runner.manipulation.NoTestsRemainException()
    }
    override fun run(notifier:org.junit.runner.notification.RunNotifier) {
        if(!enabled) return
        notifier.fireTestStarted(description)
        try { runBlocking { runProbe() } }
        catch(error:Throwable) { notifier.fireTestFailure(org.junit.runner.notification.Failure(description,error)) }
        finally { notifier.fireTestFinished(description) }
    }
    private suspend fun runProbe() {
        val token=checkNotNull(arguments.getString("token"));require(token.matches(Regex("[a-z0-9-]{1,60}")))
        val point=checkNotNull(arguments.getString("point"));require(point in listOf("intent","candidate","published","saf-create-intent","saf-created")+RemoteJournalScenario.points)
        val action=checkNotNull(arguments.getString("action"))
        val root=File(targetContext.filesDir,"journal-crash-$token")
        val home=File(root,"home");val path=File(root,"file-history/tasks.db")
        val journal=FileTaskJournal(path)
        val target=File(home,"target/a.txt")
        suspend fun stopAtBoundary(id:String) {
            val text="${Process.myPid()}\n$id\n$point\n"
            val pending=File(root,"ready.pending")
            FileOutputStream(pending).use { it.write(text.toByteArray());it.fd.sync() }
            check(pending.renameTo(File(root,"ready")))
            CompletableDeferred<Unit>().await()
        }
        when(action) {
            "stage"->{
                if(point in RemoteJournalScenario.points) {
                    RemoteJournalScenario(targetContext,root).perform(point,::stopAtBoundary)
                    error("required remote crash checkpoint was not reached")
                }
                check(!root.exists());check(File(home,"source").mkdirs());check(File(home,"target").mkdir())
                File(home,"source/a.txt").writeText("process death bytes ☃\n")
                val source=FileLocation(FileEndpoint.APP,"source");val destination=FileLocation(FileEndpoint.APP,"target")
                val browser=LocalFileBrowser(home,targetContext.contentResolver)
                val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",browser.browse(source).entries),destination,"target")
                journal.load();journal.accept(request)
                if(point=="intent") stopAtBoundary(request.id)
                val scan=FileTreeScanner(browser::browse).scan(request)
                val access=FileDestinationAccess(home,targetContext.contentResolver,browser::browse)
                val decisions=FilePasteDecisions.begin(FilePastePreflight(access::inspect,access::relation).check(scan))
                val plan=decisions.confirm(decisions.state().reference)
                journal.phase(request.id,FileJournalPhase.CONFIRMING)
                val run=FileCopyExecutor(home,targetContext.contentResolver,File(root,"staging"),{ r ->
                    FileTaskConnections(r.source.endpoint,r.destination.endpoint,{ _,_->error("no remote") },{ _,_,_->error("no auth") })
                },record={ value,checkpoint ->
                    // Published result deliberately has NOT committed to the journal yet.
                    if(point=="published" && value.state==FileCopyItemState.COMPLETE) stopAtBoundary(request.id)
                    journal.checkpoint(request.id,value,checkpoint)
                    if(point=="candidate" && checkpoint==FileCopyCheckpoint.LOCAL_CREATE) stopAtBoundary(request.id)
                }).prepare(plan)
                journal.begin(request.id,run.outcome.items)
                run.execute();error("required crash checkpoint was not reached")
            }
            "verify"->{
                val ready=File(root,"ready").readLines();check(ready.size==3 && ready[2]==point)
                check(ready[0].toInt()!=Process.myPid()) { "probe did not restart in a new process" }
                val app=object:Application() { init { attachBaseContext(targetContext) };override fun getFilesDir()=root }
                lateinit var work:FileWorkRuntime
                instrumentation.runOnMainSync { work=FileWorkRuntime(app) }
                withTimeout(10000) { while(withContext(Dispatchers.Main) { work.tasks.journalLoading }) delay(10) }
                val record=withContext(Dispatchers.Main) {
                    check(work.tasks.journalError==null) { work.tasks.journalError ?: "journal error" }
                    check(work.tasks.tasks.isEmpty()) { "recovered intent must never become executable" }
                    check(work.tasks.history.size==1) { "restored history must be visible" }
                    work.tasks.history.single()
                }
                check(record.intent.id==ready[1] && record.interrupted) { "restarted record must retain identity and interruption" }
                when(point) {
                    in RemoteJournalScenario.points -> RemoteJournalScenario(targetContext,root).verify(point,record)
                    "intent"->{ check(record.phase==FileJournalPhase.QUEUED && record.items.isEmpty());check(!target.exists()) }
                    "candidate"->{
                        check(record.items.single().checkpoint==FileCopyCheckpoint.LOCAL_CREATE)
                        check(!File(checkNotNull(record.items.single().value.candidatePath)).exists());check(!target.exists())
                    }
                    "saf-create-intent","saf-created"->{
                        val item=record.items.single()
                        val state=checkNotNull(targetContext.contentResolver.call(
                            android.net.Uri.parse("content://com.hanxi.ghosterm.test.publicationcontrol"),"state",null,Bundle()))
                        check(state.getInt("writes")==0 && state.getInt("deletes")==0)
                        if(point=="saf-created") {
                            check(item.checkpoint==FileCopyCheckpoint.DOCUMENT_CREATED)
                            val uri=android.net.Uri.parse(checkNotNull(item.value.documentUri))
                            check(state.getInt("creates")==1)
                            check(android.provider.DocumentsContract.getDocumentId(uri)==state.getString("latest"))
                            check(state.getByteArray("bytes")!!.isEmpty())
                            targetContext.contentResolver.openInputStream(uri)!!.use { check(it.read()==-1) }
                        } else {
                            check(item.checkpoint==FileCopyCheckpoint.DOCUMENT_CREATE && item.value.documentUri==null)
                            check(state.getInt("creates")==0 && state.getString("latest")==null)
                        }
                    }
                    "published"->{
                        check(record.items.single().checkpoint==FileCopyCheckpoint.PUBLISH)
                        check(record.items.single().value.state!=FileCopyItemState.COMPLETE)
                        check(target.readText()=="process death bytes ☃\n")
                    }
                }
                check(File(home,"source/a.txt").readText()=="process death bytes ☃\n")
                journal.remove(record.intent.id);check(journal.load().isEmpty())
                if(point in RemoteJournalScenario.points) RemoteJournalScenario(targetContext,root).verify(point,record)
                check(target.exists()==(point=="published")) { "clearing history must not change files" }
                val output="JOURNAL_CRASH_VERIFIED point=$point oldPid=${ready[0]} newPid=${Process.myPid()} task=${record.intent.id} target=${target.exists()}\n"
                instrumentation.sendStatus(0,Bundle().apply { putString("stream",output) })
            }
            "cleanup"->{ check(root.deleteRecursively()) }
            else->error("unknown probe action")
        }
    }
}
