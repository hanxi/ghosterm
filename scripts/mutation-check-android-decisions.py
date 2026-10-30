#!/usr/bin/env python3
"""Request-scoped decisions, immutable plans and bounded names: real device assertions, same APK config control and SHA restore."""
import argparse,hashlib,importlib.util,os,signal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_DECISIONS_MUTATION'
spec=importlib.util.spec_from_file_location('helper',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[ROOT/'build/android-toolchain/sdk/platform-tools/adb','-s',serial];require(run([*adb,'get-state']))
    domain=ROOT/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FilePasteDecisions.kt'
    originals={domain:domain.read_bytes()}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Existing decisions mutation marker')
    work=ROOT/'tmp/android-decisions-mutations';work.mkdir(exist_ok=True)
    cases=[
        ('lists-frozen',domain,'Collections.unmodifiableList(ArrayList(items))','ArrayList(items)','FilePasteDecisionsTest#plansAndStateExposeOnlyFrozenCollections'),
        ('choices-frozen',domain,'Collections.unmodifiableMap(LinkedHashMap(choices))','LinkedHashMap(choices)','FilePasteDecisionsTest#plansAndStateExposeOnlyFrozenCollections'),
        ('checked',domain,'check.checked && check.scan.complete','true && check.scan.complete','FilePasteDecisionsTest#incompleteDeniedAndFailedPreflightCannotBecomePlans'),
        ('complete',domain,'check.checked && check.scan.complete','check.checked && true','FilePasteDecisionsTest#incompleteDeniedAndFailedPreflightCannotBecomePlans'),
        ('denied',domain,'check.create!=FileCreateCapability.DENIED','true','FilePasteDecisionsTest#incompleteDeniedAndFailedPreflightCannotBecomePlans'),
        ('request-id',domain,'require(expected==ref())','require(expected.copy(requestId=request.id)==ref())','FilePasteDecisionsTest#staleForeignAndRepeatedConfirmationCannotChangeRequest'),
        ('session-id',domain,'require(expected==ref())','require(expected.copy(sessionId=sessionId)==ref())','FilePasteDecisionsTest#staleForeignAndRepeatedConfirmationCannotChangeRequest'),
        ('revision',domain,'require(expected==ref())','require(expected.copy(revision=revision)==ref())','FilePasteDecisionsTest#staleForeignAndRepeatedConfirmationCannotChangeRequest'),
        ('sealed',domain,'check(!cancelled && !confirmed)','check(!cancelled)','FilePasteDecisionsTest#staleForeignAndRepeatedConfirmationCannotChangeRequest'),
        ('cancelled',domain,'check(!cancelled && !confirmed)','check(!confirmed)','FilePasteDecisionsTest#initialPoliciesAndCancelRemainRequestLocal'),
        ('pending',domain,'check(pending().isEmpty())','check(true)','FilePasteDecisionsTest#staleForeignAndRepeatedConfirmationCannotChangeRequest'),
        ('initial-skip',domain,'FileConflictPolicy.SKIP -> choices[question.source.relativePath]=FilePasteChoice.SKIP','FileConflictPolicy.SKIP -> choices[question.source.relativePath]=FilePasteChoice.KEEP_BOTH','FilePasteDecisionsTest#initialPoliciesAndCancelRemainRequestLocal'),
        ('unsupported-choice',domain,'require(choice==FilePasteChoice.SKIP && !applyToRemaining)','require(!applyToRemaining)','FilePasteDecisionsTest#applyRemainingPreservesPriorChoicesAndNeverSilentlySkipsUnsupported'),
        ('unsupported-all',domain,'require(choice==FilePasteChoice.SKIP && !applyToRemaining)','require(choice==FilePasteChoice.SKIP)','FilePasteDecisionsTest#applyRemainingPreservesPriorChoicesAndNeverSilentlySkipsUnsupported'),
        ('prior-choice',domain,'it.kind==FilePasteDecisionKind.CONFLICT && it.source.relativePath !in choices','it.kind==FilePasteDecisionKind.CONFLICT','FilePasteDecisionsTest#applyRemainingPreservesPriorChoicesAndNeverSilentlySkipsUnsupported'),
        ('all-kind',domain,'it.kind==FilePasteDecisionKind.CONFLICT && it.source.relativePath !in choices','it.source.relativePath !in choices','FilePasteDecisionsTest#applyRemainingPreservesPriorChoicesAndNeverSilentlySkipsUnsupported'),
        ('all-policy',domain,'remainingPolicy=if(choice==FilePasteChoice.KEEP_BOTH) FileConflictPolicy.KEEP_BOTH else FileConflictPolicy.SKIP','remainingPolicy=FileConflictPolicy.ASK','FilePasteDecisionsTest#applyRemainingPreservesPriorChoicesAndNeverSilentlySkipsUnsupported'),
        ('covered-child',domain,"path.contains('/') && choices[path.substringBefore('/')]==FilePasteChoice.SKIP",'false','FilePasteDecisionsTest#directorySkipIncludesDescendantsAndCanBeRevisedBeforeConfirmation'),
        ('skip-reason',domain,'questionsByPath.getValue(decisionPath).kind','FilePasteDecisionKind.UNSUPPORTED','FilePasteDecisionsTest#directorySkipIncludesDescendantsAndCanBeRevisedBeforeConfirmation'),
        ('incoming-name',domain,'occupied.add(it.entry.name)','false','FilePasteDecisionsTest#realPlanReservesIncomingNamesAndDoesNotWrite'),
        ('rebase',domain,'byRoot.getValue(root).destinationName+node.relativePath.removePrefix(root)','node.relativePath','FilePasteDecisionsTest#opaqueDirectoryIdsAndEmptyFoldersSurviveRootRebasing'),
        ('empty-folder',domain,'planned.add(FileCopyEntry(node,path))','if(node.childCount!=0) planned.add(FileCopyEntry(node,path))','FilePasteDecisionsTest#opaqueDirectoryIdsAndEmptyFoldersSurviveRootRebasing'),
        ('unknown-total',domain,'if(unknownSizeFiles==0) knownBytes else null','knownBytes','FilePasteDecisionsTest#unknownSizesOverflowAndSkippedTotalsDoNotInventProgress'),
        ('known-sum',domain,'known+=size','known+=0uL','FilePasteDecisionsTest#unknownSizesOverflowAndSkippedTotalsDoNotInventProgress'),
        ('overflow',domain,'check(ULong.MAX_VALUE-known>=size)','check(true)','FilePasteDecisionsTest#unknownSizesOverflowAndSkippedTotalsDoNotInventProgress'),
        ('metadata',domain,'check(metadata<=maxMetadataBytes)','check(true)','FilePasteDecisionsTest#failedPlanningLeavesDecisionsEditableAndHonorsUtf8Budget'),
        ('utf8-metadata',domain,'metadata+=path.toByteArray(Charsets.UTF_8).size','metadata+=path.length','FilePasteDecisionsTest#failedPlanningLeavesDecisionsEditableAndHonorsUtf8Budget'),
        ('duplicate-path',domain,'require(nodes.size==scan.entries.size)','require(true)','FilePasteDecisionsTest#malformedTreeOwnershipRootsAndConflictMetadataAreRejected'),
        ('parent-order',domain,'parent in seen && owner?.entry?.kind==FileKind.DIRECTORY','owner?.entry?.kind==FileKind.DIRECTORY','FilePasteDecisionsTest#malformedTreeOwnershipRootsAndConflictMetadataAreRejected'),
        ('parent-identity',domain,'node.parent.directoryId==owner.entry.id','true','FilePasteDecisionsTest#malformedTreeOwnershipRootsAndConflictMetadataAreRejected'),
        ('source-endpoint',domain,'require(node.parent.endpoint==request.source.endpoint)','require(true)','FilePasteDecisionsTest#malformedTreeOwnershipRootsAndConflictMetadataAreRejected'),
        ('root-parent',domain,'require(node.parent==request.source)','require(true)','FilePasteDecisionsTest#malformedTreeOwnershipRootsAndConflictMetadataAreRejected'),
        ('path-basename',domain,'segments.last()==node.entry.name','true','FilePasteDecisionsTest#malformedTreeOwnershipRootsAndConflictMetadataAreRejected'),
        ('child-count',domain,'require(node.childCount!=null && node.childCount==(children[node.relativePath]?:0))','require(true)','FilePasteDecisionsTest#malformedTreeOwnershipRootsAndConflictMetadataAreRejected'),
        ('target-names',domain,'require(check.targetEntries.map { it.name }.toSet().size==check.targetEntries.size)','require(true)','FilePasteDecisionsTest#malformedTreeOwnershipRootsAndConflictMetadataAreRejected'),
        ('conflict-binding',domain,'require(check.conflicts==expected)','require(true)','FilePasteDecisionsTest#malformedTreeOwnershipRootsAndConflictMetadataAreRejected'),
        ('name-occupied',domain,'if(candidate !in occupied) return candidate','return candidate','FilePasteDecisionsTest#candidateNamesHandleExtensionsUnicodeAndBoundedExhaustion'),
        ('extension-type',domain,'kind==FileKind.FILE && dot>0','dot>0','FilePasteDecisionsTest#candidateNamesHandleExtensionsUnicodeAndBoundedExhaustion'),
        ('dotfile',domain,'dot>0 && original.substring(dot)','dot>=0 && original.substring(dot)','FilePasteDecisionsTest#candidateNamesHandleExtensionsUnicodeAndBoundedExhaustion'),
        ('name-bytes',domain,'if(bytes+size>budget) break','if(false && bytes+size>budget) break','FilePasteDecisionsTest#candidateNamesHandleExtensionsUnicodeAndBoundedExhaustion'),
        ('name-surrogate',domain,'require(!Character.isLowSurrogate(c))','require(true)','FilePasteDecisionsTest#candidateNamesHandleExtensionsUnicodeAndBoundedExhaustion'),
        ('attempt-bound',domain,'require(maxAttempts in 1..10000)','require(true)','FilePasteDecisionsTest#candidateNamesHandleExtensionsUnicodeAndBoundedExhaustion'),
    ]
    if args.case:
        selected=set(args.case.split(','))
        if not selected.issubset({case[0] for case in cases}):raise RuntimeError('Unknown case')
        cases=[case for case in cases if case[0] in selected]
    prefix='com.hanxi.ghosterm.'
    def build(label):
        result=run(['bash',ROOT/'scripts/build-android.sh'],900)
        (work/(label+'-build.log')).write_text(result.stdout+result.stderr);require(result)
        for apk in ('apk/debug/app-debug.apk','apk/androidTest/debug/app-debug-androidTest.apk'):
            require(run([*adb,'install','-r','-t',ROOT/'android/app/build/outputs'/apk]))
    def test(label,selection,green,count=1):
        result=run([*adb,'shell','am','instrument','-w','-r','-e','class',','.join(prefix+n for n in selection.split(',')),'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        output=result.stdout+result.stderr;(work/(label+'.log')).write_text(output)
        okay=result.returncode==0 and f'OK ({count} '+('test)' if count==1 else 'tests)') in output
        if okay!=green or (not green and ('FAILURES' not in output or not any(s in output for s in ('AssertionError','ComparisonFailure')))):
            print(output,flush=True);raise RuntimeError('Expected compiled assertion '+('green' if green else 'red'))
    build('baseline');test('baseline','FilePasteDecisionsTest',True,13)
    control='ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    test('baseline-control',control,True)
    try:
        for label,path,before,after,method in cases:
            clean=originals[path].decode()
            start,end=0,len(clean)
            piece=clean[start:end]
            if piece.count(before)!=1:raise RuntimeError(label+': missing/ambiguous anchor '+str(piece.count(before)))
            path.write_text(clean[:start]+piece.replace(before,after)+clean[end:]+'\n// '+MARK+'\n')
            target=method
            build(label);test(label,target,False);test(label+'-control',control,True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored',target,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Source restoration failed')
    print(f'ANDROID DECISIONS MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    signal.signal(signal.SIGINT,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
