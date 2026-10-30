#!/usr/bin/env python3
"""Local no-overwrite publication: real device assertions, same APK config control and SHA restore."""
import argparse,hashlib,importlib.util,os,signal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_PUBLICATION_MUTATION'
spec=importlib.util.spec_from_file_location('helper',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[ROOT/'build/android-toolchain/sdk/platform-tools/adb','-s',serial];require(run([*adb,'get-state']))
    store=ROOT/'android/data/src/main/java/com/hanxi/ghosterm/data/LocalFilePublisher.kt'
    native=ROOT/'android/bridge/src/main/cpp/publication_jni.c'
    originals={p:p.read_bytes() for p in (store,native)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Existing publication mutation marker')
    work=ROOT/'tmp/android-publication-mutations';work.mkdir(exist_ok=True)
    complete='completeBytesEmptyFileAndNestedUnicodePublishWithoutLeaking'
    raw='nativeCandidateRequiresCompleteReadonlyOwnedFile'
    paths='pathsSymlinkParentsAndInvalidNamesCannotEscapeHome'
    cancel='cancellationAndCheckpointFailureDiscardUnpublishedCandidate'
    replaced='replacedStagingPathIsNeitherPublishedNorRemoved'
    changed='changedSnapshotAndIncompleteCandidateCannotPublish'
    conflict='existingFileDirectoryAndDanglingLinkAreNeverOverwritten'
    rejected='renameRejectionAndUnsafeStagingRemainExplicit'
    cases=[
        ('exclusive',native,'O_CREAT|O_EXCL|O_RDWR','O_CREAT|O_RDWR',raw),
        ('noreplace',native,',RENAME_NOREPLACE)',',0)',conflict),
        ('root-nofollow',native,'fd=open(base,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)','fd=open(base,O_RDONLY|O_DIRECTORY|O_CLOEXEC)',paths),
        ('parent-nofollow',native,'openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)','openat(fd,part,O_RDONLY|O_DIRECTORY|O_CLOEXEC)',paths),
        ('parent-traversal',native,'strcmp(name,"..")','1',paths),
        ('mode',native,'O_CLOEXEC,0600);','O_CLOEXEC,0400);',raw),
        ('cloexec',native,'O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW|O_CLOEXEC','O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW',raw),
        ('directory-cloexec',native,'fd=open(base,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)','fd=open(base,O_RDONLY|O_DIRECTORY|O_NOFOLLOW)',raw),
        ('directory-close',native,'close(fd);fd=next;','fd=next;',complete),
        ('size',native,'if(!error && st.st_size!=size)error=EINVAL;','if(!error && st.st_size!=size && 0)error=EINVAL;',raw),
        ('readonly',native,'else if((flags&O_ACCMODE)!=O_RDONLY)error=EINVAL;','else if((flags&O_ACCMODE)!=O_RDONLY && 0)error=EINVAL;',raw),
        ('identity',native,'actual->st_ino!=named.st_ino','(actual->st_ino!=named.st_ino && 0)',replaced),
        ('cleanup-identity',native,'int error=component(value)?identity(directory,value,candidate,&st):EINVAL;','(void)st;(void)candidate;int error=component(value)?0:EINVAL;',replaced),
        ('bytes',store,'Os.write(writer.fileDescriptor,buffer,offset,read-offset)','Os.write(writer.fileDescriptor,ByteArray(buffer.size),offset,read-offset)',complete),
        ('offset',store,'size-copied).toInt(),copied)','size-copied).toInt(),0)',complete),
        ('source-change',store,'check(before.st_size==after.st_size &&','check(true || before.st_size==after.st_size &&',changed),
        ('target-size',store,'check(Os.fstat(writer.fileDescriptor).st_size==size)','check(true)',changed),
        ('writer-close',store,'writer.close();writer=null','writer=null',complete),
        ('cleanup',store,'val error=LocalPublicationFile.remove(stage.fd,owned.fd,candidateName)','val error=0',cancel),
        ('cancel-before-rename',store,'writer.close();writer=null\n                cancellation.ensureActive()','writer.close();writer=null',cancel),
        ('conflict',store,'OsConstants.EEXIST->State.CONFLICT','OsConstants.EEXIST->State.PUBLISHED',conflict),
        ('rejection',store,'else->State.REJECTED','else->State.PUBLISHED',rejected),
        ('reader-close',store,'listOf(writer,reader,stage,directory)','listOf(writer,stage,directory)',complete),
        ('private-staging',store,'check(staging.canonicalFile!=home.canonicalFile && !staging.canonicalPath.startsWith(home.canonicalPath+File.separator))','check(true)',rejected),
        ('late-cancel-preserves',store,'return snapshot.withDescriptor',
         'currentCoroutineContext()[kotlinx.coroutines.Job]?.invokeOnCompletion { cause -> if(cause!=null) File(File(home,directoryId),name).delete() }\n        return snapshot.withDescriptor',
         'cancelledAcknowledgementDoesNotDeletePublishedFile'),
    ]
    if args.case:
        cases=[case for case in cases if case[0]==args.case]
        if not cases:raise RuntimeError('Unknown case')
    prefix='com.hanxi.ghosterm.'
    def build(label):
        result=run(['bash',ROOT/'scripts/build-android.sh'],900)
        (work/(label+'-build.log')).write_text(result.stdout+result.stderr);require(result)
        for apk in ('apk/debug/app-debug.apk','apk/androidTest/debug/app-debug-androidTest.apk'):
            require(run([*adb,'install','-r','-t',ROOT/'android/app/build/outputs'/apk]))
    def test(label,selection,green,count=1):
        result=run([*adb,'shell','am','instrument','-w','-r','-e','class',prefix+selection,'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        output=result.stdout+result.stderr;(work/(label+'.log')).write_text(output)
        okay=result.returncode==0 and f'OK ({count} '+('test)' if count==1 else 'tests)') in output
        if okay!=green or (not green and ('FAILURES' not in output or not any(s in output for s in ('AssertionError','ComparisonFailure')))):
            print(output,flush=True);raise RuntimeError('Expected compiled assertion '+('green' if green else 'red'))
    build('baseline');test('baseline','LocalPublicationTest',True,10)
    control='ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    test('baseline-control',control,True)
    try:
        for label,path,before,after,method in cases:
            clean=originals[path].decode()
            start,end=0,len(clean)
            piece=clean[start:end]
            if piece.count(before)!=1:raise RuntimeError(label+': missing/ambiguous anchor '+str(piece.count(before)))
            path.write_text(clean[:start]+piece.replace(before,after)+clean[end:]+'\n// '+MARK+'\n')
            target='LocalPublicationTest#'+method
            build(label);test(label,target,False);test(label+'-control',control,True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored',target,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Source restoration failed')
    print(f'ANDROID LOCAL PUBLICATION MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    signal.signal(signal.SIGINT,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
