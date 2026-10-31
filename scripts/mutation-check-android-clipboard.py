#!/usr/bin/env python3
"""File clipboard selection and product lifecycle: real device assertions, same APK config control and SHA restore."""
import argparse,hashlib,importlib.util,os,signal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_CLIPBOARD_MUTATION'
spec=importlib.util.spec_from_file_location('helper',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[ROOT/'build/android-toolchain/sdk/platform-tools/adb','-s',serial];require(run([*adb,'get-state']))
    store=ROOT/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FileClipboard.kt'
    model=ROOT/'android/app/src/main/java/com/hanxi/ghosterm/FilesViewModel.kt'
    screen=ROOT/'android/app/src/main/java/com/hanxi/ghosterm/FilesScreen.kt'
    originals={p:p.read_bytes() for p in (store,model,screen)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Existing clipboard mutation marker')
    work=ROOT/'tmp/android-clipboard-mutations';work.mkdir(exist_ok=True)
    snapshot='snapshotOwnsImmutableMetadataAndRejectsAmbiguousSelections'
    copy='copyExitsSelectionWithoutReadingOrChangingSystemClipboard'
    replace='replacementAndClearRequireExplicitDecisionsAndKeepOriginalFiles'
    details='detailsShowEveryNameAndBackClosesOnlyTheSheet'
    cases=[
        ('owned-list',store,'Collections.unmodifiableList(ArrayList(items))','Collections.unmodifiableList(items)',snapshot),
        ('readonly-list',store,'Collections.unmodifiableList(ArrayList(items))','ArrayList(items)',snapshot),
        ('unique-items',store,'require(items.map { it.id }.toSet().size == items.size)','require(true)',snapshot),
        ('entry-budget',store,'items.size <= 4096','true',snapshot),
        ('snapshot-id',store,'UUID.randomUUID().toString()','"00000000-0000-0000-0000-000000000000"',snapshot),
        ('source-identity',store,'UUID.randomUUID().toString(), source, sourcePathLabel','UUID.randomUUID().toString(), FileLocation(FileEndpoint.APP), sourcePathLabel',snapshot),
        ('selected-only',model,'visibleEntries.filter { it.id in selected }','visibleEntries',copy),
        ('exit-selection',model,'clipboard=snapshot;clipboardError=null;dismissClipboardDialogs();clearSelection()','clipboard=snapshot;clipboardError=null;dismissClipboardDialogs()',copy),
        ('leave-preserves',model,'dismissClipboardDialogs();clipboardError=null\n        cancelRead()', 'dismissClipboardDialogs();clipboardError=null;clipboard=null\n        cancelRead()','navigationLeaveAndReloadKeepFrozenOriginAndCancelOldReplacement'),
        ('replace-confirmation',model,'else clipboardReplacement=ClipboardReplacement(previous.id,snapshot)','else installClipboard(snapshot)',replace),
        ('clear',model,'clipboard=null;clipboardDetails=false;clipboardError=null','clipboard=clipboard;clipboardDetails=false;clipboardError=null',replace),
        ('card-source',screen,'Text(clipboardSource(snapshot),maxLines=','Text("wrong source",maxLines=',copy),
        ('card-path',screen,'Text(snapshot.sourcePathLabel,maxLines=','Text(model.location.directoryId,maxLines=',copy),
        ('selection-card',screen,'model.clipboard?.let { snapshot ->','model.clipboard?.takeUnless { model.selecting }?.let { snapshot ->',replace),
        ('details-expanded',screen,'rememberModalBottomSheetState(skipPartiallyExpanded=true)','rememberModalBottomSheetState(skipPartiallyExpanded=false)',details),
        ('details-all',screen,'items(clipboard.items,key=','items(clipboard.items.take(3),key=',details),
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
    build('baseline');test('baseline','FileClipboardTest,FileClipboardLifecycleTest',True,7)
    control='ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    test('baseline-control',control,True)
    try:
        for label,path,before,after,method in cases:
            clean=originals[path].decode()
            start,end=0,len(clean)
            piece=clean[start:end]
            if piece.count(before)!=1:raise RuntimeError(label+': missing/ambiguous anchor '+str(piece.count(before)))
            path.write_text(clean[:start]+piece.replace(before,after)+clean[end:]+'\n// '+MARK+'\n')
            target='FileClipboardTest#'+method
            build(label);test(label,target,False);test(label+'-control',control,True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored',target,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Source restoration failed')
    print(f'ANDROID CLIPBOARD MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    signal.signal(signal.SIGINT,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
