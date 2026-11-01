#!/usr/bin/env python3
"""SAF text preview mutations with actual system grants, source descriptors and same-APK Core control."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_SAF_PREVIEW_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', type=int, action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise RuntimeError('Set ANDROID_SERIAL to the preview test device')
    adb = ['adb', '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    source = ROOT / 'android/data/src/main/java/com/hanxi/ghosterm/data/LocalFileBrowser.kt'
    store = ROOT / 'android/data/src/main/java/com/hanxi/ghosterm/data/TransferStagingStore.kt'
    originals = {path:path.read_bytes() for path in (source,store)}
    modes = {path: path.stat().st_mode & 0o777 for path in originals}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual SAF preview mutation')
    cases = [
        ('absolute-asset-offset','SafPreviewAssetsTest#segmentUsesAbsoluteOffsetAndNeverReadsOutsideItsDeclaredBounds',((source,'Os.lseek(fd,offset,OsConstants.SEEK_SET)','Os.lseek(fd,0,OsConstants.SEEK_SET)',1),)),
        ('declared-segment-bound','SafPreviewAssetsTest#segmentUsesAbsoluteOffsetAndNeverReadsOutsideItsDeclaredBounds',((source,'val cap=if(length>=0) length.toInt() else data.size','val cap=data.size',1),)),
        ('declared-fragment-completeness','SafPreviewAssetsTest#prematureEofCannotDisplayPartialUtf8AsACompleteDocument',((source,'check(length<0 || used.toLong()==length)','check(true)',1),)),
        ('reliable-producer-error','SafPreviewAssetsTest#reliablePipeFailureCannotBecomeSuccessfulText',((source,'asset.parcelFileDescriptor.checkError()','Unit',1),)),
        ('pipe-eof-acknowledgement','SafPreviewAssetsTest#declaredPipeWaitsForProducerCloseBeforeClaimingSuccess',((source,'if(pipe && length>=0) check(receive(used,1)==0)','if(false) check(receive(used,1)==0)',1),)),
        ('nonseekable-offset','SafPreviewAssetsTest#pipeOffsetAndSpecialDescriptorsAreExplicitlyRejected',((source,'else check(offset==0L)','else check(true)',1),)),
        ('descriptor-kind','SafPreviewAssetsTest#pipeOffsetAndSpecialDescriptorsAreExplicitlyRejected',((source,'check(regular || pipe)','check(true)',1),)),
        ('declared-logical-size','SafPreviewAssetsTest#logicalSizeMismatchDoesNotSilentlyChangeTheDocumentBeingPreviewed',(
            (source,'check(length<0 || expected==null || length==expected)','check(true)',1),
            (source,'check(expected==null || used.toLong()==expected)','check(true)',1),)),
        ('asset-range-validity','SafPreviewAssetsTest#invalidAssetRangeIsExplicitlyRejected',((source,'check(length>=-1 && offset>=0)','check(true)',1),)),
        ('unknown-descriptor-logical-size','SafPreviewAssetsTest#unknownDescriptorLengthStillChecksTheCurrentLogicalSize',((source,'check(expected==null || used.toLong()==expected)','check(true)',1),)),
        ('asset-owner-close','SafPreviewAssetsTest#regularPreviewClosesProviderDescriptorsAfterSuccessAndReadFailure',(
            (source,'class LocalFileBrowser(private val root: File, private val resolver: ContentResolver) : FileBrowserSource, PreviewFileSource {',
                'class LocalFileBrowser(private val root: File, private val resolver: ContentResolver) : FileBrowserSource, PreviewFileSource {\n    private val leakedPreviewAssets=mutableListOf<android.content.res.AssetFileDescriptor>()',1),
            (source,'checkNotNull(resolver.openAssetFileDescriptor(uri,"r",signal)) { "无法打开文档" }.use { asset ->',
                'checkNotNull(resolver.openAssetFileDescriptor(uri,"r",signal)) { "无法打开文档" }.let { asset ->\n                leakedPreviewAssets.add(asset)',1),)),
    ]
    cases.extend([
        ('raw-finite-pipe-eof','SafPreviewAssetsTest#rawFinitePipeSnapshotWaitsForProducerCompletion',((store,'if(OsConstants.S_ISFIFO(before.st_mode) && length>=0)','if(false)',1),)),
        ('raw-empty-pipe-eof','SafPreviewAssetsTest#rawEmptyPipeSnapshotDoesNotTreatZeroBytesAsProducerSuccess',((store,'if(OsConstants.S_ISFIFO(before.st_mode) && length>=0)','if(false)',1),)),
        ('raw-pipe-extra-bytes','SafPreviewAssetsTest#rawFinitePipeRejectsBytesBeyondTheDeclaredLength',((store,'check(tail==0)','check(true)',1),)),
        ('raw-asset-range','SafPreviewAssetsTest#rawSnapshotRejectsInvalidAssetRanges',((store,'check(length>=-1 && asset.startOffset>=0)','check(true)',1),)),
    ])
    selected = args.selected or list(range(1, len(cases) + 1))
    if len(selected) != len(set(selected)) or any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        for path, old, new, count in cases[number - 1][2]:
            if originals[path].decode().count(old) != count:
                raise RuntimeError(f'C{number}: source anchor missing or ambiguous')
    work = ROOT / 'tmp/android-saf-preview-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    for path, data in originals.items():
        (work / path.name).write_bytes(data)
    print('EXTERNAL PREVIEW RECEIPTS', work, flush=True)

    def write_source(path, data):
        temporary = work / ('source-' + str(time.time_ns()) + '.kt')
        temporary.write_bytes(data)
        temporary.chmod(modes[path])
        os.replace(temporary, path)

    def restore():
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes():
                write_source(path, data)
            if path.read_bytes() != data:
                raise RuntimeError('External preview source restore SHA256 mismatch: ' + path.name)

    def run_logged(label, command, seconds=120):
        result = helper.run(command, seconds)
        output = result.stdout + result.stderr
        (work / (label + '.log')).write_text(output)
        helper.require(result)
        return output

    def build(label):
        run_logged(label + '-build', ['bash', ROOT / 'scripts/build-android.sh'], 900)
        for name in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
            run_logged(label + '-install-' + Path(name).name, [*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / name])

    def test(label, target, green):
        output = run_logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
            'com.hanxi.ghosterm.' + target, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        if green:
            good = 'OK (1 test)' in output and 'FAILURES' not in output
        else:
            good = bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and any(
                kind in output for kind in ('java.lang.AssertionError', 'org.junit.ComparisonFailure'))
            good = good and 'Fixture:' not in output
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output or not good:
            raise RuntimeError(output)

    results = []
    try:
        build('baseline')
        for index, target in enumerate(dict.fromkeys(cases[number - 1][1] for number in selected)):
            test('baseline-' + str(index), target, True)
        for number in selected:
            name, target, edits = cases[number - 1]
            variants = {}
            for path, old, new, count in edits:
                variants[path] = variants.get(path, originals[path].decode()).replace(old, new)
            for path, source in variants.items():
                write_source(path, (source + ('\n<!-- ' + MARK + ' -->\n' if path.suffix == '.xml' else '\n// ' + MARK + '\n')).encode())
            label = f'C{number:02d}-{name}'
            build(label)
            test(label + '-target', target, False)
            test(label + '-control', 'ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', True)
            test(label + '-preview-control', 'SafPreviewAssetsTest#normalUnknownLengthPipeStaysReadable', True)
            if any(path == store for path, *_ in edits):
                test(label + '-finite-control', 'SafPreviewAssetsTest#rawFinitePipeStillProducesTheExactSuccessfulBytes', True)
            restore()
            build(label + '-restored')
            test(label + '-restored', target, True)
            results.append(dict(case=number, name=name, compiled_assertion_red=True, same_apk_core_green=True, restored_green=True))
            (work / 'accepted-results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(label + ': compiled single assertion red / same APK Core green / restored green / SHA256', flush=True)
    finally:
        restore()
        helper.require(helper.run([*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm']))
    print(f'ANDROID SAF PREVIEW MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
