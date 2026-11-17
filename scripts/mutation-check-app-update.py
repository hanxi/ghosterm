#!/usr/bin/env python3
"""Compile and mutate isolated macOS update sources; keep the user's checkout intact."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MODEL = 'apple/GhostermApp/Sources/GhostermAppModel/UpdateCheck.swift'
ABOUT = 'apple/GhostermApp/Sources/GhostermApp/AboutWindow.swift'
NATIVE = 'apple/GhostermApp/Sources/GhostermApp/NativeUpdater.swift'
CONTROL = 'HostListTests.testReadingDoesNotMutateConfig'


def main():
    cases = [
        ('unknown', MODEL, 'guard let raw = local.commit, raw != "unknown" else { return .cannotTell(.localCommitUnknown) }',
         'guard let raw = local.commit, raw != "unknown" else { return .upToDate }',
         'UpdateCheckTests.testUnknownLocalCommitIsNotReportedUpToDate'),
        ('commit', MODEL, 'return theirs.hasPrefix(mine) ? .upToDate : .newer(remote)',
         'return local.version == remote.version ? .upToDate : .newer(remote)',
         'UpdateCheckTests.testBuildNumberAloneNeverDecides'),
        ('missing-signature-kind', MODEL, 'signature = try text(.signature, "signature")',
         'signature = (raw["signature"] as? String) ?? "id"',
         'UpdateCheckTests.testManifestRejectsMissingOrMalformedFields'),
        ('unsigned-install', MODEL,
         'public var installBlocked: Bool { updateSignature == nil || updateVersion?.allSatisfy(\\.isNumber) != true || updateVersion?.isEmpty != false }',
         'public var installBlocked: Bool { false }', 'UpdateCheckTests.testUnsignedUpdateIsBlocked'),
        ('status', MODEL, 'case .cannotTell(let reason): return "查不出来：\\(reason.text)"',
         'case .cannotTell: return "已是最新"', 'UpdateCheckTests.testCannotTellNeverWearsUpToDateClothing'),
        ('about-entry', ABOUT, 'UpdateRow(updater: updater)', 'EmptyView()',
         'AboutWindowTests.testAboutWindowDrawsUpdateEntryAndNoInstallButton'),
        ('premature-install', ABOUT,
         'Button(updater.isChecking ? "检查中…" : "检查更新") { updater.checkNow() }',
         'Button("安装更新") {}\n                Button(updater.isChecking ? "检查中…" : "检查更新") { updater.checkNow() }',
         'AboutWindowTests.testUpdateRowAloneKeepsExactlyOneActionAlongsideTheNewLicenseEntry'),
        ('progress', NATIVE, 'received: received + length', 'received: length',
         'NativeUpdaterTests.testDownloadProgressCancellationAndHiddenNoticeKeepSharedState'),
        ('forced-restart', NATIVE, '        readyReply = reply', '        reply(.install)\n        readyReply = reply',
         'NativeUpdaterTests.testReadyWaitsForExplicitRestartAndCanRetryAfterTerminationIsCancelled'),
        ('release-order', MODEL, 'remote.version.compare(mine, options: .numeric) == .orderedDescending',
         'remote.version.compare(mine, options: .numeric) == .orderedAscending',
         'UpdateCheckTests.testReleaseUsesStableVersionAndIgnoresWorkflowNumbers'),
    ]
    originals = {path: (ROOT / path).read_bytes() for path in [MODEL, ABOUT, NATIVE]}
    assert all(b'GT_MUTATION_MARK' not in value for value in originals.values()), 'residual mutation marker'
    env = dict(os.environ)
    if Path('/Applications/Xcode.app/Contents/Developer').is_dir():
        env.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    with tempfile.TemporaryDirectory(prefix='mutation-app-update-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        source = work / 'source'
        for folder in ['apple/GhostermApp', 'apple/GhostermCore', 'apple/GhostermTerminal']:
            shutil.copytree(ROOT / folder, source / folder, symlinks=True,
                            ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        (source / 'core').mkdir()
        for name in ['include', 'build']:
            (source / 'core' / name).symlink_to(ROOT / 'core' / name, target_is_directory=True)
        for name in ['build', 'patches', 'plugins']:
            (source / name).symlink_to(ROOT / name, target_is_directory=True)
        temp = work / 'temporary'; temp.mkdir()
        env['TMPDIR'] = str(temp)
        pkg = source / 'apple/GhostermApp'
        def run(label, selected, red=False):
            command = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require app-update; shift; exec "$gt_timeout_bin" --kill-after=8 420 "$@"',
                       'app-update', str(ROOT), 'swift', 'test', '--package-path', str(pkg), '--filter', selected]
            result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            output = result.stdout.decode(errors='replace')
            (work / (label + '.log')).write_text(output)
            if red:
                assert result.returncode != 0 and re.search(r'Executed 1 test, with [1-9][0-9]* failures? \(0 unexpected\)', output), (label, output[-10000:])
                assert re.search(r': error: .* : (XCTAssert\w+ failed|failed -)', output), (label, output[-10000:])
                assert not any(s in output for s in ['unexpected signal', 'AddressSanitizer', 'runtime error:']), label
            else:
                assert result.returncode == 0 and re.search(r'Executed \d+ tests?, with 0 failures', output), (label, output[-10000:])
                assert 'skipped' not in output.lower(), label
            print(label + ': ' + ('normal assertion red' if red else 'green'), flush=True)
        run('baseline', 'UpdateCheckTests|NativeUpdaterTests|AboutWindowTests')
        for label, path, old, new, selected in cases:
            target = source / path
            text = originals[path].decode()
            assert text.count(old) == 1, (label, 'anchor drift')
            target.write_text(text.replace(old, new, 1) + '\n// GT_MUTATION_MARK\n')
            try:
                run(label, selected, True)
                run(label + '-control', CONTROL)
            finally:
                target.write_bytes(originals[path])
            assert target.read_bytes() == originals[path]
            run(label + '-restored', selected)
        run('final', 'UpdateCheckTests|NativeUpdaterTests|AboutWindowTests')
    for path, data in originals.items():
        assert (ROOT / path).read_bytes() == data, 'original source changed concurrently: ' + path
        print(path + ' SHA256 ' + hashlib.sha256(data).hexdigest())
    print('APP UPDATE MUTATIONS PASS: 10 groups; compiled assertion red / independent green / restored green / SHA256')


if __name__ == '__main__':
    main()
