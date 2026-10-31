#!/usr/bin/env python3
"""Compiled copy executor mutations; real HOME/SAF/SSH IO and same-APK config control."""
import hashlib
import os

MARK = 'GT_ANDROID_COPY_EXECUTOR_MUTATION'

def execute(root, build, test):
    executor=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileCopyExecutor.kt'
    progress=root/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FileCopyProgress.kt'
    store=root/'android/data/src/main/java/com/hanxi/ghosterm/data/TransferStagingStore.kt'
    originals={p:p.read_bytes() for p in (executor,progress,store)}
    if any(MARK.encode() in value for value in originals.values()):
        raise RuntimeError('Residual copy executor mutation')
    local='com.hanxi.ghosterm.FileCopyExecutorTest#'
    saf='com.hanxi.ghosterm.SafCopyExecutorTest#'
    tree=local+'localTreeCopiesRealBytesEmptyHiddenAndUnicodeAndKeepsSource'
    conflict=local+'lateConflictsAskSkipAndKeepBothNeverOverwrite'
    skips=local+'directorySkipIncludesAllChildrenAndMixedUnsupportedIsVisible'
    late=saf+'cancellationWaitsForLateCreatedDirectoryAndKeepsItsIdentity'
    readback=saf+'cancelledCallerCannotDiscardReadbackSideEffectsOrReleaseEarly'
    cases=[
        ('initial-scan',executor,'val scan=rescan()','val scan=plan.check.scan',local+'frozenPlanAndSourceChangesStopBeforeDestinationWrites'),
        ('final-scan',executor,'active();rescan();verified=true','active();verified=true',saf+'changedSourceAfterPublicationKeepsResultButCannotClearClipboard'),
        ('incoming-names',executor,'occupied.addAll(reserved)','Unit',conflict),
        ('planned-name',executor,'var name=if(top) names.getValue(root) else source.entry.name','var name=source.entry.name',local+'plannedRenameUsesFrozenNameAndRetainsActualResult'),
        ('rename-budget',executor,'occupied,renameAttempts)','occupied,10000)',local+'renameAndSnapshotBudgetsStopWithoutOverwriting'),
        ('snapshot-budget',executor,'File(privateRoot,"source"),fileLimit,fileLimit','File(privateRoot,"source"),256L*1024*1024,256L*1024*1024',local+'renameAndSnapshotBudgetsStopWithoutOverwriting'),
        ('all-skipped',executor,'if(!plan.hasWork) return@launch','if(false) return@launch',local+'allSkippedDoesNotAcquireConnectionsOrCreateStaging'),
        ('skip-descendants',executor,'skipped.add(root);report','Unit;report',skips),
        ('initial-skips',executor,'if(node.relativePath in skips) FileCopyItemState.SKIPPED else','if(false) FileCopyItemState.SKIPPED else',skips),
        ('immutable-results',progress,'Collections.unmodifiableList(ArrayList(items))','ArrayList(items)',tree),
        ('clipboard-complete-only',progress,'state==FileCopyState.COMPLETE','state==FileCopyState.COMPLETE || state==FileCopyState.WITH_SKIPS',skips),
        ('committed-bytes',progress,'it.state==FileCopyItemState.COMPLETE && it.source.entry.kind==FileKind.FILE','it.source.entry.kind==FileKind.FILE',readback),
        ('request-id',executor,'FileCopyProgress(plan.request.id,plan.request.clipboardId','FileCopyProgress(plan.id,plan.request.clipboardId',tree),
        ('clipboard-id',executor,'plan.request.id,plan.request.clipboardId','plan.request.id,plan.request.id',tree),
        ('one-shot',executor,'check(!finalized.get() && started.compareAndSet(false,true))','check(true)',local+'cancellationBeforeStartAndRepeatedExecutionCannotWriteAgain'),
        ('actual-parent',executor,'created[source.relativePath]=checkNotNull(out.location)','created[source.relativePath]=parent',saf+'nestedSafTargetsUseActualNamesAndOpaqueParentIds'),
        ('actual-name',executor,'targetName=out.actualName ?: name,documentUri=out.documentUri','targetName=name,documentUri=out.documentUri',saf+'nestedSafTargetsUseActualNamesAndOpaqueParentIds'),
        ('created-result',executor,'FileDirectoryCreator.State.CREATED->FileCopyItemState.CREATED','FileDirectoryCreator.State.CREATED->FileCopyItemState.NOT_ATTEMPTED',late),
        ('unknown-result',executor,'SafFilePublisher.State.UNKNOWN->FileCopyItemState.UNKNOWN','SafFilePublisher.State.UNKNOWN->FileCopyItemState.FAILED',saf+'lostCreationReplyIsUnknownAndNeverRetriedOrDeleted'),
        ('cancelled-caller',executor,'waiting.invokeOnCancellation { cancel() }','waiting.invokeOnCancellation { }',readback),
        ('cancel-operation',executor,'cancelOperation.get()?.let','cancelOperation.get()?.takeIf { false }?.let',late),
        ('source-role',executor,'scope.remote(Role.SOURCE,source.parent.endpoint)','scope.remote(Role.DESTINATION,source.parent.endpoint)',local+'realSshCopiesBothDirectionsAndKeepsBrowserIndependent'),
        ('remote-publication',executor,'destination.publish(candidate,targetId,expectedSize=size)','RemotePublicationResult(0,RemotePublicationState.PUBLISHED,0u)',local+'realSshCopiesBothDirectionsAndKeepsBrowserIndependent'),
        ('local-publication',executor,'publish(parent.directoryId,name,snapshot)','publish(parent.directoryId,"wrong-"+name,snapshot)',tree),
        ('close-await',store,'close();released.await()','close();Unit',local+'sourceStoreCloseWaitsForBlockedProviderAndItsLateDescriptor'),
        ('lease-await',store,'closed && slots==0 && receivers.isEmpty() && sourceReads.isEmpty()','closed && receivers.isEmpty() && sourceReads.isEmpty()',local+'sourceStoreCloseKeepsOutstandingReaderReservationUntilItReturns'),
        ('directory-core-code',executor,'coreStatus=out.coreStatus','coreStatus=null',local+'remoteRefusalPreservesCodesAndCandidateWithoutTouchingTarget'),
        ('directory-remote-code',executor,'remoteStatus=out.remoteStatus','remoteStatus=null',local+'remoteRefusalPreservesCodesAndCandidateWithoutTouchingTarget'),
        ('creation-core-code',executor,'coreStatus=staged.status','coreStatus=null',local+'remoteRefusalPreservesCodesAndCandidateWithoutTouchingTarget'),
        ('publication-code',executor,'coreStatus=published.status','coreStatus=null',local+'realSshCopiesBothDirectionsAndKeepsBrowserIndependent'),
        ('candidate-path',executor,'result=result.copy(candidatePath=candidate)','result=result.copy(candidatePath=null)',local+'remoteRefusalPreservesCodesAndCandidateWithoutTouchingTarget'),
        ('per-item-source',executor,
            ['current.location==source.parent && current.entries.singleOrNull { it.id==source.entry.id }==source.entry',
             'after.location==source.parent && after.entries.singleOrNull { it.id==source.entry.id }==source.entry'],
            ['true','true'],saf+'changedFollowingSourceIsRejectedBeforeItsDestinationIsCreated'),
        ('source-signal',store,'pending.second.forEach { signal -> Dispatchers.IO.dispatch(EmptyCoroutineContext) { signal.cancel() } }','Unit',local+'storeCloseSignalsBlockedSourceAndCancelledWaiterDoesNotReopenIt'),
    ]
    selected=os.environ.get('ANDROID_COPY_EXECUTOR_MUTATIONS','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}: raise RuntimeError('Unknown copy executor mutation')
        cases=[c for c in cases if c[0] in selected]
    def restore():
        for path,value in originals.items():
            if MARK.encode() in path.read_bytes(): path.write_bytes(value)
        if any(hashlib.sha256(path.read_bytes()).digest()!=hashlib.sha256(value).digest() for path,value in originals.items()):
            raise RuntimeError('Copy executor restoration failed')
    try:
        for name,path,old,new,method in cases:
            text=originals[path].decode()
            pairs=zip(old,new) if isinstance(old,list) else [(old,new)]
            for before,after in pairs:
                assert text.count(before)==1,(name,text.count(before))
                text=text.replace(before,after)
            path.write_text(text+'\n// '+MARK+'\n')
            label='copy-executor-'+name
            build(label);test(label,method,False)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            restore();build(label+'-restored');test(label+'-restored',method,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally: restore()
    build('copy-executor-final')
    test('copy-executor-final','com.hanxi.ghosterm.FileCopyExecutorTest,com.hanxi.ghosterm.SafCopyExecutorTest',True)
    print(f'ANDROID COPY EXECUTOR MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    raise SystemExit('Use ANDROID_COPY_EXECUTOR_MUTATE=1 with test-android-ssh.sh and FileCopyExecutorTest,SafCopyExecutorTest')
