#!/usr/bin/env python3
"""Real local/SAF browser: compiled mutation red, same-APK config green, restored green/SHA."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import signal

ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_FILES_MUTATION'
spec=importlib.util.spec_from_file_location('android_mutation_helpers',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case',type=int,help='One group; omitted runs all groups')
    args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL to the intended test device')
    adb=[str(ROOT/'build/android-toolchain/sdk/platform-tools/adb'),'-s',serial]
    require(run([*adb,'get-state']))
    model=ROOT/'android/app/src/main/java/com/hanxi/ghosterm/FilesViewModel.kt'
    source=ROOT/'android/data/src/main/java/com/hanxi/ghosterm/data/LocalFileBrowser.kt'
    screen=ROOT/'android/app/src/main/java/com/hanxi/ghosterm/FilesScreen.kt'
    app=ROOT/'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    originals={p:p.read_bytes() for p in (model,source,screen,app)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Existing mutation marker')
    work=ROOT/'tmp/android-files-mutations';work.mkdir(exist_ok=True)
    test_class='com.hanxi.ghosterm.'
    lifecycle='TerminalLifecycleTest#filesNavigationPreservesOwnerPidAndResetsTerminalInputState'
    control='ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    ui='FileBrowserTest#tapOpensAndLongPressSelectsWithoutChangingDirectory'
    selection='FileBrowserTest#selectAllUsesVisibleRowsAndHiddenTogglePrunesSelection'
    late='FileBrowserTest#navigationAndPreviewIgnoreCancelledLateResponses'
    preview='FileBrowserTest#previewRejectsOversizeMalformedAndNulWithoutWritingAnything'
    grant='FilesSafTest#realTreeGrantReadsOpaqueIdsSurvivesRotationAndDetectsRevocation'
    cases=[
        (model,'if(selecting) { selected=','if(true) { selected=',ui),
        (model,'selecting=true;selected=selected+entry.id','selecting=false;selected=selected+entry.id',ui),
        (model,'selected=visibleEntries.map { it.id }.toSet()','selected=entries.map { it.id }.toSet()',selection),
        (model,'selected=selected.intersect(visibleEntries.map { it.id }.toSet())','selected=selected',selection),
        (model,'error=failure.message ?: "读取文件夹失败，请重试"','error=null','FileBrowserTest#readFailureIsVisibleAndRetryDoesNotPretendEmpty'),
        (model,'if(visible && token==generation) {\n                    crumbs=', 'if(visible) {\n                    crumbs=',late),
        (model,'if(visible && token==generation) preview=FilePreview(entry,text)','if(visible) preview=FilePreview(entry,text)',late),
        (source,'resolver.takePersistableUriPermission(selected,granted)','Unit',grant),
        (source,'check(resolver.persistedUriPermissions.any { it.uri == uri && it.isReadPermission })',
         'check(true || resolver.persistedUriPermissions.any { it.uri == uri && it.isReadPermission })',grant),
        (source,'check(used<=PREVIEW_LIMIT)','check(true)','FilesSafTest#providerErrorsEmptyUnknownSizeAndCancelledStreamRemainDistinct'),
        (source,'.onMalformedInput(CodingErrorAction.REPORT)','.onMalformedInput(CodingErrorAction.REPLACE)',preview),
        (source,'check((0 until used).none { data[it]==0.toByte() })','check(true)',preview),
        (source,'if(index<parts.lastIndex) check(OsConstants.S_ISDIR(Os.lstat(path.path).st_mode))',
         'if(false && index<parts.lastIndex) check(OsConstants.S_ISDIR(Os.lstat(path.path).st_mode))',
         'FileBrowserTest#linksTraversalAndSpecialFilesAreExplicitlyRejected'),
        (source,'check(entries.size<ENTRY_LIMIT)','check(true)','FileBrowserTest#tooManyEntriesReportsFailureRatherThanPartialSuccess'),
        (app,'TerminalScreen(sessions, files::enter)',
         'TerminalScreen(sessions) { sessions.active?.let { sessions.close(it.id) }; files.enter() }',lifecycle),
        (screen,'onLongClick={ model.longPress(entry) }','onLongClick={ model.tap(entry) }',ui),
        (source,'check(active()) { "预览已取消" }','check(true) { "预览已取消" }',
         'FilesSafTest#providerErrorsEmptyUnknownSizeAndCancelledStreamRemainDistinct'),
        (source,'// This owner opened it with Os.open, so it must also close it.\n                Os.close(fd)','// This owner opened it with Os.open, so it must also close it.\n                Unit','FileBrowserTest#localPreviewClosesDescriptorsAfterSuccessAndDecodeFailure'),
    ]
    if args.case and not 1<=args.case<=len(cases):raise RuntimeError('Invalid case')
    def build(label):
        result=run(['bash',ROOT/'scripts/build-android.sh'],seconds=900)
        (work/f'{label}-build.log').write_text(result.stdout+result.stderr);require(result)
        for apk in ('apk/debug/app-debug.apk','apk/androidTest/debug/app-debug-androidTest.apk'):
            require(run([*adb,'install','-r','-t',ROOT/'android/app/build/outputs'/apk]))
    def test(names,green,label,count=1):
        result=run([*adb,'shell','am','instrument','-w','-r','-e','class',
                    ','.join(test_class+n for n in names.split(',')),
                    'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'],seconds=300)
        output=result.stdout+result.stderr;(work/f'{label}.log').write_text(output)
        passed=result.returncode==0 and f'OK ({count} '+('test)' if count==1 else 'tests)') in output
        if passed!=green or (not green and ('FAILURES' not in output or not any(x in output for x in ('AssertionError','org.junit.ComparisonFailure')))):
            raise RuntimeError(output)
    build('baseline')
    test('FileBrowserTest,FilesSafTest,'+lifecycle,True,'baseline',16)
    test(control,True,'baseline-control')
    try:
        for index,(path,before,after,target) in enumerate(cases,1):
            if args.case and args.case!=index:continue
            clean=originals[path].decode()
            if clean.count(before)!=1:raise RuntimeError(f'F{index}: missing/ambiguous anchor')
            path.write_text(clean.replace(before,after+' /* '+MARK+' */'))
            build(f'F{index}-mutated');test(target,False,f'F{index}-red');test(control,True,f'F{index}-control')
            path.write_bytes(originals[path])
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('SHA restore mismatch')
            build(f'F{index}-restored');test(target,True,f'F{index}-green')
            print(f'F{index}: target red / same-APK control green / restored green / SHA256 identical',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Sources changed during mutations')
    print(f'ANDROID FILES MUTATIONS PASS: {1 if args.case else len(cases)} groups',flush=True)


if __name__=='__main__':
    def interrupt(signum,frame):raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT,interrupt);signal.signal(signal.SIGTERM,interrupt)
    main()
