#!/usr/bin/env python3
"""Durable source digests and explicit historical content observations; invoke through the SSH fixture."""
import hashlib
import os
import re

MARK = 'GT_ANDROID_CONTENT_MUTATION'

def execute(root,build,test):
    digest=root/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FileHistoryObservation.kt'
    snapshot=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileContentHasher.kt'
    executor=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileCopyExecutor.kt'
    codec=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileJournalCodec.kt'
    inspector=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileHistoryContentInspector.kt'
    model=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksViewModel.kt'
    screen=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksScreen.kt'
    private=root/'android/data/src/main/java/com/hanxi/ghosterm/data/LocalRetainedFiles.kt'
    budget=root/'android/data/src/main/java/com/hanxi/ghosterm/data/LocalPublicationBudget.kt'
    journal=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileTaskJournal.kt'
    local_origin=root/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/LocalFileIdentity.kt'
    creator=root/'android/data/src/main/java/com/hanxi/ghosterm/data/LocalCreationMarker.kt'
    publisher=root/'android/data/src/main/java/com/hanxi/ghosterm/data/LocalFilePublisher.kt'
    identity_reader=root/'android/data/src/main/java/com/hanxi/ghosterm/data/LocalFileIdentityReader.kt'
    originals={p:p.read_bytes() for p in (digest,snapshot,executor,codec,inspector,model,screen,private,budget,journal,local_origin,creator,publisher,identity_reader)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual content mutation')
    data='FileContentTest#'
    cases=[
        ('hash-bytes',snapshot,'hash.update(buffer,0,count)','Unit',data+'realSnapshotDigestKeepsPublisherOffsetAndCapacityLease'),
        ('hash-cancel',snapshot,'checkpoint()','Unit',data+'digestCheckpointCancellationReleasesReaderWithoutDestroyingSnapshot'),
        ('digest-before-write',executor,'result=result.copy(sourceDigest=snapshot.contentDigest { active() })','snapshot.contentDigest { active() }',data+'digestIsDurableBeforeCreationAndRetainedAcrossNewJournal'),
        ('codec-digest',codec,'val digest=if(version=="2" || version=="3")','val digest=if(false)',data+'digestIsDurableBeforeCreationAndRetainedAcrossNewJournal'),
        ('comparison',inspector,'if(actual==expected)','if(actual!=expected)',data+'historicalMatchMismatchAndMissingNeverPromoteUnknownResult'),
        ('no-reference',inspector,'if(expected==null) return result','if(false) return result',data+'legacyWithoutReferenceDoesNotReadOrGuess'),
        ('metadata-change',inspector,'check(after.location==parent && after.entries.singleOrNull { it.id==entry.id }==entry)','check(true)',data+'changedMetadataAndReadFailureCannotReportMatchingBytes'),
        ('unknown-error',inspector,'result(FileHistoryContentState.UNKNOWN,error=error.message','result(FileHistoryContentState.MATCH,error=error.message',data+'changedMetadataAndReadFailureCannotReportMatchingBytes'),
        ('screen-action',screen,'onContent(item,candidate)','Unit','FileJournalProductTest#explicitContentVerificationShowsMatchAndDifferenceWithoutChangingOldResult'),
        ('digest-shape',digest,'sha256.matches(Regex("[0-9a-f]{64}"))','true',data+'digestIsDurableBeforeCreationAndRetainedAcrossNewJournal'),
    ]
    old='val observed=withTimeout(60_000) { withContext(Dispatchers.IO) {\n                    work.authentication.fileHistoryConnections(record.intent.id,record.intent.destination.endpoint,::cancelInspection)'
    cases.append(('target-only',model,old,old.replace('record.intent.destination.endpoint','record.intent.source.endpoint'),
        'RemoteFileTasksProductTest#historicalContentUsesFreshDestinationAndStoredDigestAfterSourceDisappears'))
    cases.append(('chunk-progress',snapshot,'hash.update(buffer,0,count);offset+=count','hash.update(buffer,0,count);offset=size',
        data+'multiChunkBinaryDigestIncludesEveryByte'))
    owner='FileJournalProductTest#sharedContentLeaseQueuesCopiesAndRejectsAnotherReaderOrLateRelease'
    cases += [
        ('reader-single',model,'if(busy || journalLoading || contentReadOwner!=null) return null','if(busy || journalLoading) return null',owner),
        ('reader-queue',model,'if(contentReadOwner!=null) return','if(false) return',owner),
        ('reader-release',model,'if(contentReadOwner!==reference) return false','if(false) return false',owner),
    ]
    cases += [
        ('reader-vm-release',model,'work.tasks.endContentRead(readReference)','Unit','FileJournalProductTest#explicitContentVerificationShowsMatchAndDifferenceWithoutChangingOldResult'),
        ('reader-history-remove',model,'if(record !in history || journalLoading || busy || contentReadOwner!=null) return','if(record !in history || journalLoading || busy) return',owner),
    ]
    private_test='LocalRetainedContentTest#'
    shape='require(parts.size==3 && canonical(parts[0]) && parts[1]=="destination" && canonical(parts[2]) &&\n            File(root,parts.joinToString("/")).absolutePath==referencePath)'
    fresh=originals[private].decode()
    fresh=fresh[fresh.index('                            try {\n                                LocalPublicationFile.directory(root,"").use { freshRoot ->',fresh.index('suspend fun content(')):]
    fresh=fresh[:fresh.index('                            context.ensureActive();result')]
    cases += [
        ('private-shape',private,shape,'require(true)',private_test+'noncanonicalForeignAndSourcePathsNeverReadAnything'),
        ('private-limit',private,'check(before.size<=byteLimit)','check(true)',private_test+'limitAndCancellationCloseAllHandlesWithoutChangingTheFile'),
        ('private-stability',private,'fingerprint(Os.fstat(file.fileDescriptor))==before','true',private_test+'observableContentChangeDuringReadIsUnknown'),
        ('private-binding',private,fresh,'',private_test+'replacedDirectoryNameCannotTurnTheOldPinnedFileIntoACurrentMatch'),
        ('private-idle',budget,'check(tasks.isEmpty())','check(true)',private_test+'activeCopyRejectsPrivateReadsAndNeverChangesItsCandidate'),
        ('private-missing',private,'if(!exists(destination,parts[2]))return@whileIdle null','if(!exists(destination,parts[2]))error("missing")',private_test+'historyComparesPrivateCandidateWithoutInferringItsTaskOwnership'),
        ('private-dispatch',inspector,"if(candidate && parent?.endpoint==FileEndpoint.APP && item.value.candidatePath?.startsWith('/')==true)",'if(false)',private_test+'historyComparesPrivateCandidateWithoutInferringItsTaskOwnership'),
        ('private-vm',model,'retained={ path -> LocalRetainedFiles(File(app.filesDir,"file-tasks")).content(path) }','retained=null','FileJournalProductTest#privateCandidateContentUsesReadonlyNamespaceAndDoesNotChangeUnknownHistory'),
    ]
    plan='FileExecutionPlanTest#'
    cases += [
        ('plan-encode',codec,'header.put("plan",v.executionPlanId)','Unit',plan+'planAndLineageSurviveNewStoreAndEveryTerminalPhase'),
        ('plan-decode',codec,'executionPlanId=plan)','executionPlanId=null)',plan+'planAndLineageSurviveNewStoreAndEveryTerminalPhase'),
        ('plan-register',journal,'updatedMillis=System.currentTimeMillis(),executionPlanId=executionPlanId','updatedMillis=System.currentTimeMillis(),executionPlanId=null',plan+'planAndLineageSurviveNewStoreAndEveryTerminalPhase'),
        ('plan-unique-record',journal,'check(FileJournalCodec.readHeader(rows.getBlob(0)).executionPlanId!=executionPlanId)','check(true)',plan+'duplicateAndInvalidPlansFailBeforeAnyItemsAreInserted'),
        ('plan-unique-load',journal,'check(plans.distinct().size==plans.size)','check(true)',plan+'duplicateSavedPlansAreRejectedWithoutRepairOrDeletion'),
        ('plan-fields',codec,'o.fields(*(when(version) { "3"->fields+arrayOf("plan","retryOf");"2"->fields+"retryOf";else->fields }))','if(version!="3") o.fields(*(when(version) { "3"->fields+arrayOf("plan","retryOf");"2"->fields+"retryOf";else->fields }))',plan+'missingPlanAndUnknownFieldsCannotDowngradeToLegacy'),
        ('plan-uuid',codec,'val plan=if(version=="3") uuid(o.text("plan")) else null','val plan=if(version=="3") o.text("plan") else null',plan+'registeredPlanRequiresCanonicalIdentityAndAnExecutionPhase'),
        ('plan-phase',codec,'require(plan==null || phase in setOf(FileJournalPhase.RUNNING,FileJournalPhase.CANCELLING,FileJournalPhase.FINISHED))','require(true)',plan+'registeredPlanRequiresCanonicalIdentityAndAnExecutionPhase'),
        ('plan-vm',model,'journal.begin(task.request.id,execution.outcome.items,plan.id)','journal.begin(task.request.id,execution.outcome.items,null)','FileJournalProductTest#finishedProductPersistsResultsAndNewRuntimeDoesNotReplayCopy'),
        ('plan-item',private,'stamp.size,millis,task)','stamp.size,millis,"")','RetainedFilesProductTest#registeredPlanFindsItsRetainedCandidateWithoutACompletedItemCheckpoint'),
        ('plan-label',model,'history.filter { it.executionPlanId==item.executionPlanId }','history.filter { false }','RetainedFilesProductTest#registeredPlanFindsItsRetainedCandidateWithoutACompletedItemCheckpoint'),
    ]
    identity='LocalFileIdentityTest#'
    producer=identity+'creationReferenceCommitsBeforePayloadAndSurvivesPublication'
    ui_identity='FileJournalProductTest#identityVerificationDistinguishesIdenticalReplacementWithoutChangingCopyResult'
    order=originals[publisher].decode()
    order=order[order.index('                created(identity)'):order.index('                writer.close();writer=null')+len('                writer.close();writer=null')]
    fresh=originals[identity_reader].decode()
    fresh=fresh[fresh.index('                        val fresh=openParent()'):]
    fresh=fresh[:fresh.index('                        ensureActive()')]
    cases += [
        ('origin-marker',creator,'Os.setxattr(pinned,NAME,bytes,CREATE_ONLY)','Unit',producer),
        ('origin-exclusive',creator,'private const val CREATE_ONLY=1','private const val CREATE_ONLY=0',identity+'createOnlyMarkerCannotReplaceAnExistingReference'),
        ('origin-order',publisher,order,order.replace('created(identity)','Unit')+'\n                created(identity)',producer),
        ('origin-record',executor,'report(result,FileCopyCheckpoint.LOCAL_CREATED);active()','active()',identity+'refusedCreationReferenceStopsBeforePayloadOrPublication'),
        ('origin-encode',codec,'i.localIdentity?.let { r -> body.put("local",obj','i.localIdentity?.takeIf { false }?.let { r -> body.put("local",obj',producer),
        ('origin-decode',codec,'?.toUInt(),digest,identity)','?.toUInt(),digest,null)',producer),
        ('origin-token-shape',local_origin,'UUID.fromString(token).toString()==token','true',identity+'strictCodecRejectsChangedCreationReferenceWithoutRewritingIt'),
        ('origin-name-shape',local_origin,'UUID.fromString(candidateName).toString()==candidateName','true',identity+'strictCodecRejectsChangedCreationReferenceWithoutRewritingIt'),
        ('origin-uid-shape',local_origin,'uid>=0','true',identity+'strictCodecRejectsChangedCreationReferenceWithoutRewritingIt'),
        ('origin-fields',codec,'r.fields("token","candidate","device","inode","uid","candidateDevice","candidateInode","destinationDevice","destinationInode")','Unit',identity+'strictCodecRejectsChangedCreationReferenceWithoutRewritingIt'),
        ('origin-endpoint',codec,'o.optionalObject("parent")?.let(::location)?.endpoint==FileEndpoint.APP','true',identity+'codecCannotAttachCreationReferencesToForeignOrUnattemptedItems'),
        ('origin-state',codec,'FileCopyItemState.valueOf(o.text("state")) !in setOf(FileCopyItemState.NOT_ATTEMPTED,FileCopyItemState.SKIPPED)','true',identity+'codecCannotAttachCreationReferencesToForeignOrUnattemptedItems'),
        ('origin-file-id',identity_reader,'initial.st_dev!=reference.device || initial.st_ino!=reference.inode','false',identity+'copiedMarkerAndIdenticalBytesCannotIdentifyAnotherInode'),
        ('origin-token',identity_reader,'if(token!=null && token.contentEquals(reference.token.toByteArray(Charsets.US_ASCII)))','if(true)',identity+'absentOrDifferentMarkerIsNotTheRecordedCreation'),
        ('origin-parent',identity_reader,'return s.st_dev==expectedDevice && s.st_ino==expectedInode','return true',identity+'sameObjectMovedIntoAReplacementDirectoryDoesNotMatchTheOldPosition'),
        ('origin-fresh',identity_reader,fresh,'',identity+'parentReplacementWhileReadingCannotUseTheOldPinnedObject'),
        ('origin-cancel',identity_reader,'catch(cancel:CancellationException) { throw cancel }','catch(cancel:CancellationException) { result(LocalFileIdentityState.UNKNOWN) }',identity+'linksAreNeverFollowedAndCancellationReleasesEveryHandle'),
        ('origin-unknown',identity_reader,'result(LocalFileIdentityState.UNKNOWN,error.message','result(LocalFileIdentityState.DIFFERENT,error.message',identity+'unreadableObjectsRemainUnknownInsteadOfMatchingOrMissing'),
        ('origin-no-reference',identity_reader,'result(LocalFileIdentityState.NO_REFERENCE,','result(LocalFileIdentityState.SAME,',identity+'legacyAndInvalidPathsNeverReadAnotherPrivateNamespace'),
        ('origin-app-uid',identity_reader,'require(reference.uid==Process.myUid())','require(true)',identity+'legacyAndInvalidPathsNeverReadAnotherPrivateNamespace'),
        ('origin-plan',identity_reader,'require(record.executionPlanId==null || record.executionPlanId==parts[0])','require(true)',identity+'legacyAndInvalidPathsNeverReadAnotherPrivateNamespace'),
        ('origin-record-endpoint',identity_reader,'parent.endpoint==FileEndpoint.APP && record.intent.destination.endpoint==FileEndpoint.APP','true',identity+'legacyAndInvalidPathsNeverReadAnotherPrivateNamespace'),
        ('origin-created-id',identity_reader,'require(item.value.createdId==null || item.value.createdId==id)','require(true)',identity+'legacyAndInvalidPathsNeverReadAnotherPrivateNamespace'),
        ('origin-screen',screen,'onIdentity(item,candidate)','Unit',ui_identity),
        ('origin-vm-release',model,'work.tasks.endContentRead(identityReference)','Unit',ui_identity),
        ('origin-vm-route',model,'LocalFileIdentityReader(File(app.filesDir,"home"),File(app.filesDir,"file-tasks")).inspect(record,item,candidate)','LocalFileIdentityReader(File(app.filesDir,"missing-home"),File(app.filesDir,"file-tasks")).inspect(record,item,candidate)',ui_identity),
        ('origin-legacy-screen',screen,'if(value.localIdentity==null)','if(false)','FileJournalProductTest#privateCandidateContentUsesReadonlyNamespaceAndDoesNotChangeUnknownHistory'),
    ]
    selected=os.environ.get('ANDROID_FILE_CONTENT_CASES','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown content mutation')
        cases=[c for c in cases if c[0] in selected]
    for name,path,old,new,target in cases:
        if originals[path].decode().count(old)!=(2 if name=='hash-cancel' else 1):raise RuntimeError('Ambiguous mutation '+name)
    def one(label,target,green):
        output=test(label,'com.hanxi.ghosterm.'+target,green)
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output:raise RuntimeError(output)
        valid=('OK (1 test)' in output and 'FAILURES' not in output) if green else (bool(re.search(r'Tests run: 1,\s+Failures: 1',output)) and any(t in output for t in ('java.lang.AssertionError','org.junit.ComparisonFailure')))
        if not valid:raise RuntimeError(output)
    try:
        for name,path,old,new,target in cases:
            changed=originals[path].decode().replace(old,new)
            if name=='private-stability':changed=changed.replace('fingerprint(Os.fstat(freshFile.fileDescriptor))==before','true')
            path.write_text(changed+'\n// '+MARK+'\n')
            label='content-'+name
            build(label);one(label,target,False)
            one(label+'-control','ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');one(label+'-restored',target,True)
            print('PASS '+label+': compiled assertion red / same APK Core green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('Content source restoration failed')
    print(f'ANDROID CONTENT MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_FILE_CONTENT_MUTATE=1 with scripts/test-android-ssh.sh')
