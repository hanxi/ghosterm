#!/usr/bin/env python3
"""SAF new-document publication: real device assertions, same APK config control and SHA restore."""
import argparse,hashlib,importlib.util,os,signal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_SAF_PUBLICATION_MUTATION'
spec=importlib.util.spec_from_file_location('helper',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[ROOT/'build/android-toolchain/sdk/platform-tools/adb','-s',serial];require(run([*adb,'get-state']))
    store=ROOT/'android/data/src/main/java/com/hanxi/ghosterm/data/SafFilePublisher.kt'
    originals={store:store.read_bytes()}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Existing SAF mutation marker')
    work=ROOT/'tmp/android-saf-publication-mutations';work.mkdir(exist_ok=True)
    complete='completeBytesAndEmptyNestedDocumentCloseAndVerify'
    grant='invalidNamesTreeIdentityAndReadOnlyGrantStopBeforeCreate'
    denied='writeDenialRetainsCreatedUriAndDoesNotDelete'
    listing='unsupportedDirectoryAndFailedOrDuplicateListingDoNotCreate'
    readback='corruptedShortExtraUnreadableAndReliableErrorNeverComplete'
    pressure='backpressuredWriterCancellationClosesLeaseAndKeepsPartialDocument'
    late='cancelledDelayedCreateKeepsLeaseUntilReplyAndRetainsUri'
    cases=[
        ('cancel-unstarted',store,'if (!started.get()) closed.complete(Unit)','Unit','cancellationBeforeCreateAndCheckpointFailureRetainTruth'),
        ('grant-write',store,'it.isReadPermission && it.isWritePermission','it.isReadPermission',grant),
        ('tree-identity',store,'check(Documents.getTreeDocumentId(tree) == endpoint.rootId)','check(true)',grant),
        ('nested-parent',store,'check(parent == root || Documents.isChildDocument(resolver, root, parent))','check(parent == root)',complete),
        ('create-capability',store,'directory.flags and Document.FLAG_DIR_SUPPORTS_CREATE != 0','true',listing),
        ('list-duplicates',store,'check(result.put(id, cursor.required(Document.COLUMN_DISPLAY_NAME)) == null)','check(result.put(id, cursor.required(Document.COLUMN_DISPLAY_NAME)) == null || true)',listing),
        ('list-limit',store,'check(result.size < 4096)','check(true)',listing),
        ('conflict',store,'Outcome(State.CONFLICT)','Outcome(State.COMPLETE)','observedConflictNeverCreatesOrWrites'),
        ('unknown-create',store,'outcome = Outcome(State.UNKNOWN)','outcome = Outcome(State.NOT_ATTEMPTED)','failedCreateReplyRemainsUnknownEvenIfDocumentWasCreated'),
        ('new-identity',store,'check(id !in existing && id != endpoint.rootId && id != location.directoryId)','check(true)','returnedExistingOrParentIdentityIsNeverOpenedOrDeleted'),
        ('late-created-uri',store,'outcome = outcome.copy(state = State.CREATED)','active();outcome = outcome.copy(state = State.CREATED)',late),
        ('write-capability',store,'check(target.flags and Document.FLAG_SUPPORTS_WRITE != 0)','check(true)',denied),
        ('new-empty',store,'check(target.size == null || target.size == 0L)','check(true)',denied),
        ('virtual-type',store,'info.flags and Document.FLAG_VIRTUAL_DOCUMENT == 0','true',denied),
        ('query-identity',store,'check(id == Documents.getDocumentId(uri))','check(true)',denied),
        ('provider-name',store,'outcome = outcome.copy(actualName = target.name)','outcome = outcome.copy(actualName = requestedName)','providerChosenNameAndPipeDestinationAreVerified'),
        ('source-bytes',store,'Os.write(fd, buffer, offset, count - offset)','Os.write(fd, ByteArray(buffer.size), offset, count - offset)',complete),
        ('source-offset',store,'size - copied).toInt(), copied)','size - copied).toInt(), 0)',complete),
        ('source-change',store,'check(before.st_size == after.st_size && before.st_mtim == after.st_mtim && before.st_ctim == after.st_ctim)','check(true)','sourceMutationStopsBeforeReadbackAndRetainsCreatedDocument'),
        ('writer-close',store,'else writer.close()','else Unit',complete),
        ('reliable-write-error',store,'writer.closeWithError("Ghosterm copy did not complete")','writer.close()',pressure),
        ('nonblocking',store,'flags or OsConstants.O_NONBLOCK','flags',pressure),
        ('poll-bound',store,'}), 100) != 0','}), -1) != 0',pressure),
        ('readback',store,'verify(created, target, size, expectedHash.digest())','Unit',readback),
        ('hash',store,'actual.digest().contentEquals(expected)','true',readback),
        ('reliable-read-error',store,'asset.parcelFileDescriptor.checkError()','Unit',readback),
        ('worker-cancel',store,'worker?.cancel()','Unit','cancellationStopsSuspendedProgressBeforeReleasingReservation'),
        ('result-retained',store,'cancelled.set(true)','cancelled.set(true);outcome = Outcome()','cancelledResultDeliveryKeepsCompleteAndRequestIsOneShot'),
        ('one-shot',store,'check(started.compareAndSet(false, true))','check(true)','cancelledResultDeliveryKeepsCompleteAndRequestIsOneShot'),
        ('source-fd-close',store,'ParcelFileDescriptor.fromFd(fd).use { input','ParcelFileDescriptor.fromFd(fd).let { input',complete),
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
        result=run([*adb,'shell','am','instrument','-w','-r','-e','class',prefix+selection,'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        output=result.stdout+result.stderr;(work/(label+'.log')).write_text(output)
        okay=result.returncode==0 and f'OK ({count} '+('test)' if count==1 else 'tests)') in output
        if okay!=green or (not green and ('FAILURES' not in output or not any(s in output for s in ('AssertionError','ComparisonFailure')))):
            print(output,flush=True);raise RuntimeError('Expected compiled assertion '+('green' if green else 'red'))
    build('baseline');test('baseline','SafPublicationTest',True,20)
    control='ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    test('baseline-control',control,True)
    try:
        for label,path,before,after,method in cases:
            clean=originals[path].decode()
            start,end=0,len(clean)
            piece=clean[start:end]
            if piece.count(before)!=1:raise RuntimeError(label+': missing/ambiguous anchor '+str(piece.count(before)))
            path.write_text(clean[:start]+piece.replace(before,after)+clean[end:]+'\n// '+MARK+'\n')
            target='SafPublicationTest#'+method
            build(label);test(label,target,False);test(label+'-control',control,True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored',target,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Source restoration failed')
    print(f'ANDROID SAF PUBLICATION MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    signal.signal(signal.SIGINT,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
