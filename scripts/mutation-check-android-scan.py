#!/usr/bin/env python3
"""Paste intent and bounded source tree scanning: real device assertions, same APK config control and SHA restore."""
import argparse,hashlib,importlib.util,os,signal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_SCAN_MUTATION'
spec=importlib.util.spec_from_file_location('helper',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[ROOT/'build/android-toolchain/sdk/platform-tools/adb','-s',serial];require(run([*adb,'get-state']))
    domain=ROOT/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FilePasteRequest.kt'
    scanner=ROOT/'android/data/src/main/java/com/hanxi/ghosterm/data/FileTreeScanner.kt'
    originals={p:p.read_bytes() for p in (domain,scanner)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Existing scan mutation marker')
    work=ROOT/'tmp/android-scan-mutations';work.mkdir(exist_ok=True)
    request='requestFreezesIndependentIdentitySourceDestinationAndPolicy'
    real='realTreePreservesEmptyHiddenUnicodeAndUnsupportedWithoutReadingOrWriting'
    sizes='unknownAndUnsignedSizesNeverBecomeZeroOrOverflowedTotals'
    changed='selectedIdentityTypeNameAndKnownAttributesAreRevalidated'
    invalid='ambiguousUnsafeOrForeignEntriesNeverProduceACompleteTree'
    limits='depthEntryMetadataAndDirectoryLimitsFailExplicitlyAtBoundaries'
    consistency='directoryRedirectionAndChangesDuringEnumerationInvalidateThePlan'
    cases=[
        ('request-id', domain, 'UUID.randomUUID().toString(), clipboard.id', '"00000000-0000-0000-0000-000000000000", clipboard.id', request),
        ('clipboard-id', domain, 'clipboard.id, clipboard.source', 'UUID.randomUUID().toString(), clipboard.source', request),
        ('destination', domain, 'destination, destinationPathLabel, conflictPolicy)', 'clipboard.source, destinationPathLabel, conflictPolicy)', request),
        ('policy', domain, 'destination, destinationPathLabel, conflictPolicy)', 'destination, destinationPathLabel, FileConflictPolicy.ASK)', request),
        ('request-owned', domain, 'Collections.unmodifiableList(ArrayList(clipboard.items))', 'clipboard.items', request),
        ('request-readonly', domain, 'Collections.unmodifiableList(ArrayList(clipboard.items))', 'ArrayList(clipboard.items)', request),
        ('scan-readonly', domain, 'Collections.unmodifiableList(ArrayList(entries))', 'ArrayList(entries)', real),
        ('unknown-total', domain, 'complete && unknownSizeFiles == 0 && unsupported == 0', 'complete && unsupported == 0', sizes),
        ('unsupported-total', domain, 'complete && unknownSizeFiles == 0 && unsupported == 0', 'complete && unknownSizeFiles == 0', real),
        ('recursion', scanner, 'if(entry.kind==FileKind.DIRECTORY) {', 'if(false && entry.kind==FileKind.DIRECTORY) {', real),
        ('empty-directory', scanner, 'copy(childCount=children.size)', 'copy(childCount=null)', real),
        ('parent-location', scanner, 'FileScanEntry(parent,entry,relative)', 'FileScanEntry(request.source,entry,relative)', real),
        ('selected-only', scanner, 'val selected=request.items.map', 'val selected=before.values.map', real),
        ('source-kind', scanner, 'current.kind!=expected.kind', '(false && current.kind!=expected.kind)', changed),
        ('source-name', scanner, 'current.name!=expected.name', '(false && current.name!=expected.name)', changed),
        ('source-size', scanner, 'expected.size!=null && current.size!=expected.size', 'false && expected.size!=null && current.size!=expected.size', changed),
        ('source-time', scanner, 'expected.modifiedMillis!=null && current.modifiedMillis!=expected.modifiedMillis', 'false && expected.modifiedMillis!=null && current.modifiedMillis!=expected.modifiedMillis', changed),
        ('location', scanner, 'if(result.location!=location)', 'if(false && result.location!=location)', consistency),
        ('duplicate-id', scanner, 'entries.map { it.id }.toSet().size!=entries.size', '(false && entries.map { it.id }.toSet().size!=entries.size)', invalid),
        ('duplicate-name', scanner, 'entries.map { it.name }.toSet().size!=entries.size', '(false && entries.map { it.name }.toSet().size!=entries.size)', invalid),
        ('unsafe-name', scanner, "'/' in entry.name", "(false && '/' in entry.name)", invalid),
        ('unicode', scanner, 'Character.isLowSurrogate(c)', '(false && Character.isLowSurrogate(c))', invalid),
        ('membership', scanner, 'if(expected!=null && entry.id!=expected)', 'if(false && expected!=null && entry.id!=expected)', invalid),
        ('depth', scanner, 'depth>limits.maxDepth', '(false && depth>limits.maxDepth)', limits),
        ('entries', scanner, 'found.size>=limits.maxEntries', '(false && found.size>=limits.maxEntries)', limits),
        ('metadata', scanner, 'if(metadata+cost>limits.maxMetadataBytes)', 'if(false && metadata+cost>limits.maxMetadataBytes)', limits),
        ('page-size', scanner, 'if(entries.size>4096)', 'if(false && entries.size>4096)', limits),
        ('size-overflow', scanner, 'ULong.MAX_VALUE-known<size', '(false && ULong.MAX_VALUE-known<size)', sizes),
        ('repeat-directory', scanner, 'if(!visited.add(entry.id))', 'if(false && !visited.add(entry.id))', 'repeatedOpaqueDirectoryIsAnErrorRatherThanSilentlySkipped'),
        ('directory-recheck', scanner, 'if(children.associateBy { it.id }!=after.associateBy { it.id })', 'if(false && children.associateBy { it.id }!=after.associateBy { it.id })', consistency),
        ('root-recheck', scanner, 'for(entry in selected) { at=entry.name;unchanged(entry,after[entry.id]) }', 'for(entry in emptyList<BrowserEntry>()) { at=entry.name;unchanged(entry,after[entry.id]) }', consistency),
        ('read-error', scanner, 'failure=FileScanFailure(FileScanFailureKind.READ,at,error.message ?: "来源读取失败，扫描不完整")', 'failure=null', 'readFailureRetainsPartialEntriesButEmptyDirectoryIsComplete'),
        ('timeout', scanner, 'if(finished==null) failure=', 'if(false && finished==null) failure=', 'deadlineReturnsIncompleteAndCancelsTheActiveRead'),
        ('cancel-read', scanner, 'val result=browse(location)', 'val result=kotlinx.coroutines.withContext(kotlinx.coroutines.NonCancellable) { browse(location) }', 'cancellationReachesActiveReadAndNeverDeliversAPlan'),
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
    build('baseline');test('baseline','FileTreeScannerTest',True,11)
    control='ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    test('baseline-control',control,True)
    try:
        for label,path,before,after,method in cases:
            clean=originals[path].decode()
            start,end=0,len(clean)
            piece=clean[start:end]
            if piece.count(before)!=1:raise RuntimeError(label+': missing/ambiguous anchor '+str(piece.count(before)))
            path.write_text(clean[:start]+piece.replace(before,after)+clean[end:]+'\n// '+MARK+'\n')
            target='FileTreeScannerTest#'+method
            build(label);test(label,target,False);test(label+'-control',control,True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored',target,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Source restoration failed')
    print(f'ANDROID SCAN MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    signal.signal(signal.SIGINT,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
