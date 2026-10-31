#!/usr/bin/env python3
"""Directory creation, non-overwrite and late provider results: real device assertions, same APK config control and SHA restore."""
import argparse,hashlib,importlib.util,os,signal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_DIRECTORIES_MUTATION'
spec=importlib.util.spec_from_file_location('helper',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[ROOT/'build/android-toolchain/sdk/platform-tools/adb','-s',serial];require(run([*adb,'get-state']))
    domain=ROOT/'android/data/src/main/java/com/hanxi/ghosterm/data/FileDirectoryCreator.kt'
    native=ROOT/'android/bridge/src/main/cpp/publication_jni.c'
    originals={p:p.read_bytes() for p in (domain,native)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Existing directories mutation marker')
    work=ROOT/'tmp/android-directories-mutations';work.mkdir(exist_ok=True)
    cases=[
        ('native-components',native,'if(!component(value))error=EINVAL;\n    else if(mkdirat','if(0)error=EINVAL;\n    else if(mkdirat','LocalDirectoryTest#nativeComponentValidationAndPinnedParentDoNotEscape'),
        ('native-conflict',native,'else if(mkdirat(directory,value,0700)<0)error=errno;','else if(mkdirat(directory,value,0700)<0)error=0;','LocalDirectoryTest#existingObjectsRemainConflictsIncludingDanglingLinks'),
        ('native-target',native,'mkdirat(directory,value,0700)','mkdirat(directory,"misdirected",0700)','LocalDirectoryTest#nativeComponentValidationAndPinnedParentDoNotEscape'),
        ('native-private',native,'mkdirat(directory,value,0700)','mkdirat(directory,value,0600)','LocalDirectoryTest#createsNestedEmptyDirectoriesAndCopiesRealBytes'),
        ('name-components',domain,'name.isNotEmpty() && name!="." && name!=".." && \'/\' !in name && \'\\u0000\' !in name','true','LocalDirectoryTest#invalidNamesAndRemoteEndpointsAreRejectedBeforeIo'),
        ('name-bytes',domain,'remaining()<=255','remaining()<=255000','LocalDirectoryTest#invalidNamesAndRemoteEndpointsAreRejectedBeforeIo'),
        ('endpoint',domain,'parent.endpoint==FileEndpoint.APP || (parent.endpoint.treeUri!=null && parent.endpoint.remote==null)','true','LocalDirectoryTest#invalidNamesAndRemoteEndpointsAreRejectedBeforeIo'),
        ('one-shot',domain,'check(started.compareAndSet(false,true))','check(true)','LocalDirectoryTest#cancellationBeforeStartAndSealedRequestDoNotRepeatIo'),
        ('cancel-start',domain,'if(cancelled.get()) owner.cancel() else owner.start()','owner.start()','SafDirectoryTest#cancellationBeforeStartAndRepeatedExecutionDoNotCreate'),
        ('local-conflict-state',domain,'if(error==OsConstants.EEXIST) State.CONFLICT else State.REJECTED','State.REJECTED','LocalDirectoryTest#existingObjectsRemainConflictsIncludingDanglingLinks'),
        ('saf-grant',domain,'it.uri==tree && it.isReadPermission && it.isWritePermission','it.uri==tree && it.isReadPermission','SafDirectoryTest#deniedRevokedAndUnreadableTargetsNeverCreate'),
        ('saf-parent-routing',domain,'private suspend fun saf() {','private suspend fun saf() {\n            val parent=this.parent.copy(directoryId=this.parent.endpoint.rootId)','SafDirectoryTest#existingNamesAndInvalidTargetsNeverCreate'),
        ('saf-capability',domain,'parentInfo.directory && parentInfo.flags and Document.FLAG_DIR_SUPPORTS_CREATE!=0','parentInfo.directory','SafDirectoryTest#deniedRevokedAndUnreadableTargetsNeverCreate'),
        ('saf-conflict',domain,'if(requestedName in existing.values)','if(false)','SafDirectoryTest#existingNamesAndInvalidTargetsNeverCreate'),
        ('saf-existing-id',domain,'check(id !in existing && id!=endpoint.rootId && id!=parent.directoryId)','check(true)','SafDirectoryTest#uncertainCreationNeverRetriesOrClaimsSuccess'),
        ('saf-late-side-effect',domain,'outcome.copy(state=State.CREATED,location=parent.copy(directoryId=id))','outcome.copy(state=State.UNKNOWN,location=parent.copy(directoryId=id))','SafDirectoryTest#cancellationWaitsForLateProviderAndRetainsCreatedDirectory'),
        ('premature-complete',domain,'outcome.copy(state=State.CREATED,location=parent.copy(directoryId=id))','outcome.copy(state=State.COMPLETE,location=parent.copy(directoryId=id))','SafDirectoryTest#invalidNewDirectoryRetainsIdentityWithoutDeleting'),
        ('saf-type',domain,'actual.directory && actual.flags and Document.FLAG_VIRTUAL_DOCUMENT==0','actual.flags and Document.FLAG_VIRTUAL_DOCUMENT==0','SafDirectoryTest#invalidNewDirectoryRetainsIdentityWithoutDeleting'),
        ('saf-empty',domain,'check(children(tree,id).isEmpty())','check(true)','SafDirectoryTest#invalidNewDirectoryRetainsIdentityWithoutDeleting'),
        ('saf-metadata-id',domain,'cursor.moveToFirst() && cursor.getString(0)==Documents.getDocumentId(uri)','cursor.moveToFirst()','SafDirectoryTest#invalidNewDirectoryRetainsIdentityWithoutDeleting'),
        ('saf-actual-name',domain,'outcome=outcome.copy(actualName=actual.name)','outcome=outcome.copy(actualName=requestedName)','SafDirectoryTest#createsOpaqueNestedAndEmptyDirectoriesWithActualNames'),
        ('saf-duplicates',domain,'check(result.put(child,name)==null && names.add(name))','check(result.put(child,name)!=null || names.add(name))','SafDirectoryTest#deniedRevokedAndUnreadableTargetsNeverCreate'),
        ('saf-count',domain,'check(result.size<4096)','check(true)','SafDirectoryTest#deniedRevokedAndUnreadableTargetsNeverCreate'),
        ('saf-direct-parent',domain,'check(children(tree,parent.directoryId)[id]==actual.name)','check(true)','SafDirectoryTest#invalidNewDirectoryRetainsIdentityWithoutDeleting'),
        ('saf-metadata-budget',domain,'check(bytes<=4*1024*1024)','check(true)','SafDirectoryTest#deniedRevokedAndUnreadableTargetsNeverCreate'),
        ('local-denial',domain,'else State.REJECTED','else State.CONFLICT','LocalDirectoryTest#kernelDenialIsRejectedWithoutInventingADirectory'),
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
    build('baseline');test('baseline','LocalDirectoryTest,SafDirectoryTest',True,16)
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
    print(f'ANDROID DIRECTORIES MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    signal.signal(signal.SIGINT,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
