#!/usr/bin/env python3
"""Anonymous file staging: real device assertions, same APK config control and SHA restore."""
import argparse,hashlib,importlib.util,os,signal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_STAGING_MUTATION'
spec=importlib.util.spec_from_file_location('helper',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[ROOT/'build/android-toolchain/sdk/platform-tools/adb','-s',serial];require(run([*adb,'get-state']))
    store=ROOT/'android/data/src/main/java/com/hanxi/ghosterm/data/TransferStagingStore.kt'
    native=ROOT/'android/bridge/src/main/cpp/staging_jni.c'
    originals={p:p.read_bytes() for p in (store,native)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Existing staging mutation marker')
    work=ROOT/'tmp/android-staging-mutations';work.mkdir(exist_ok=True)
    local='localSnapshotIsAnonymousReadOnlyStableAndHasIndependentReaders'
    capacity='capacityIncludesLiveReadersAndFailureReleasesReservations'
    bounds='exactLimitUnknownLengthAndAssetSliceDoNotTruncateSilently'
    raw='nativeAllocationHasNoNameAndRejectsSymlinkDirectory'
    cases=[
        ('slots',store,'check(slots < snapshotLimit)','check(true)','emptySnapshotsStillConsumeSlotsUntilLastReaderReleases'),
        ('source-changed',store,'if(OsConstants.S_ISREG(before.st_mode)) check(before.st_size==after.st_size &&','if(false) check(before.st_size==after.st_size &&','sourceMetadataChangeDuringReadRejectsTheSnapshot'),
        ('anonymous',native,'fd=openat(directory,".",O_TMPFILE|O_RDWR|O_EXCL|O_CLOEXEC,0600);','fd=openat(directory,"mutation-named",O_CREAT|O_RDWR|O_EXCL|O_CLOEXEC,0600);',raw),
        ('mode',native,'O_CLOEXEC,0600);','O_CLOEXEC,0400);',raw),
        ('cloexec',native,'O_TMPFILE|O_RDWR|O_EXCL|O_CLOEXEC','O_TMPFILE|O_RDWR|O_EXCL',raw),
        ('nofollow',native,'O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC','O_DIRECTORY|O_CLOEXEC',raw),
        ('directory-close',native,'error=errno;close(directory);','error=errno;',raw),
        ('capacity',store,'check(amount <= totalLimit - reserved)','check(true)',capacity),
        ('disk-space',store,'check(available >= freeSpaceReserve && reservedBytes <= available - freeSpaceReserve)','check(available >= 0)','sourceLinksStaleSizeWrongParentAndNoSpaceFailBeforePublication'),
        ('size-bound',store,'check(read.toLong() <= remaining)','check(true)',bounds),
        ('asset-offset',store,'Os.lseek(source,asset.startOffset,OsConstants.SEEK_SET)','Os.lseek(source,0,OsConstants.SEEK_SET)',bounds),
        ('early-eof',store,'check(length < 0 || size == length)','check(true)',bounds),
        ('source-shrunk',store,'check(expected == null || size == expected)','check(true)','sourceLinksStaleSizeWrongParentAndNoSpaceFailBeforePublication'),
        ('pipe-error',store,'asset.parcelFileDescriptor.checkError()','Unit','reliablePipeErrorIsNotACompleteSnapshotAndStoreCloseKeepsBorrowedContent'),
        ('readonly',store,'Os.open("/proc/self/fd/${descriptor.fd}",OsConstants.O_RDONLY','Os.open("/proc/self/fd/${descriptor.fd}",OsConstants.O_RDWR',local),
        ('reader-offset',store,'Os.open("/proc/self/fd/${descriptor.fd}",OsConstants.O_RDONLY or OsConstants.O_CLOEXEC,0)','Os.dup(descriptor.fileDescriptor)',local),
        ('writer-close',store,'writer.close();writer=null','writer=null',local),
        ('source-close',store,'open(signal).use { asset ->','run { val asset=open(signal)',local),
        ('loan-close',store,'try { loan.close() } finally','try { Unit } finally',local),
        ('reader-reservation',store,'disposed && readers==0 && snapshots.remove(this)','disposed && snapshots.remove(this)',capacity),
        ('failed-reservation',store,'if(!transferred) release(reservation)','if(!transferred) Unit',capacity),
        ('cancel-poll',store,'waiting.isActive && active()','true && active()','cancelledPipeClosesOwnerAndDoesNotPublishSnapshot'),
        ('cancel-delivery',store,'onCancellation = { _, value, _ -> value.close() }','onCancellation = { _, _, _ -> Unit }','cancellationAfterCopyBeforeDeliveryClosesTheUnclaimedSnapshot'),
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
    build('baseline');test('baseline','TransferStagingTest',True,10)
    control='ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    test('baseline-control',control,True)
    try:
        for label,path,before,after,method in cases:
            clean=originals[path].decode()
            start,end=0,len(clean)
            if path==store and clean.count(before)>1:
                start=0 # local snapshot delivery and copy; remote path has independent mutations
                end=clean.index('    /** Complete remote bytes',start)
            piece=clean[start:end]
            if piece.count(before)!=1:raise RuntimeError(label+': missing/ambiguous anchor '+str(piece.count(before)))
            path.write_text(clean[:start]+piece.replace(before,after)+clean[end:]+'\n// '+MARK+'\n')
            target='TransferStagingTest#'+method
            build(label);test(label,target,False);test(label+'-control',control,True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored',target,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Source restoration failed')
    print(f'ANDROID STAGING MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    signal.signal(signal.SIGINT,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
