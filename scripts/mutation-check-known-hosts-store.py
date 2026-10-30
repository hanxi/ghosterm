#!/usr/bin/env python3
"""Atomic trust updates: real filesystem faults, locks, independent config control."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import signal
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_KNOWN_HOSTS_STORE_MUTATION'
spec = importlib.util.spec_from_file_location('helper', ROOT/'scripts/mutation-check-known-hosts-snapshot.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
run, require = helper.run, helper.require


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--apple', action='store_true')
    apple = parser.parse_args().apple
    source = ROOT/'core/src/known_hosts_store.c'
    swift = ROOT/'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift'
    originals = {p: p.read_bytes() for p in (source, swift)}
    if any(MARK.encode() in b for b in originals.values()): raise RuntimeError('residual mutation marker')
    logs = ROOT/'tmp/known-hosts-store-mutations'; logs.mkdir(parents=True, exist_ok=True)
    cases = [
        ('revision', 'if (expected && (length != expected_length || memcmp(current, expected, length)))',
         'if (false && expected && (length != expected_length || memcmp(current, expected, length)))', 'stale revision rejected'),
        ('conflict', 'result->conflict = true;', 'result->conflict = false;', 'stale revision rejected'),
        ('readonly', '(!entry.recognized || *entry.marker || entry.hashed)',
         '(!entry.recognized || entry.hashed)', 'nonordinary removal rejected'),
        ('range', 'bytes + entry.byte_end, length - entry.byte_end', 'bytes + entry.byte_end - 1, length - entry.byte_end', 'exact record range only'),
        ('backup', 'if (exists && (unique_blob(directory, "bak", current, length, backup) || fsync(directory)))',
         'if (false && exists && (unique_blob(directory, "bak", current, length, backup) || fsync(directory)))', 'required backup exists'),
        ('write-error', 'if (failure) { unlinkat(directory, name, 0);',
         'if (false && failure) { unlinkat(directory, name, 0);', 'fault backup write'),
        ('blob-sync', 'if (!failure && fsync(fd)) failure = errno;',
         'if (!failure) (void)fsync(fd);', 'fault backup sync'),
        ('directory-sync', 'unique_blob(directory, "bak", current, length, backup) || fsync(directory)',
         'unique_blob(directory, "bak", current, length, backup) || (fsync(directory), 0)', 'fault backup directory sync'),
        ('recheck', 'if (lstat(parent, &parent_now) || !same_file(&directory_identity, &parent_now) ||',
         'if (false && (lstat(parent, &parent_now) || !same_file(&directory_identity, &parent_now) ||', 'external edit conflicts'),
        ('rename-result', 'if (renameat(directory, staged, directory, name)) goto done;',
         '(void)renameat(directory, staged, directory, name);', 'fault replace'),
        ('committed', 'result->committed = true;', 'result->committed = false;', 'remove committed'),
        ('committed-sync', 'if (fsync(directory)) goto done;', '(void)fsync(directory);', 'fault committed directory sync'),
        ('read-close', 'if (close(fd) && !failure) failure = errno;\n  if (failure) { free(bytes);',
         '(void)close(fd);\n  if (failure) { free(bytes);', 'fault read close'),
        ('nofollow', 'O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK',
         'O_RDONLY | O_CLOEXEC | O_NONBLOCK', 'symlink rejected'),
        ('hardlink', 'identity->st_nlink != 1 ||', '', 'hardlink rejected'),
        ('thread-lock', 'int rc = pthread_mutex_trylock(&writer_mutex);',
         'int rc = pthread_mutex_trylock(&writer_mutex); if (rc == EBUSY) break;', 'lock contention stops mutation'),
        ('process-lock', 'while (fcntl(lock, F_SETLK, &request) < 0)',
         'while (false && fcntl(lock, F_SETLK, &request) < 0)', 'lock contention stops mutation'),
    ]
    with tempfile.TemporaryDirectory(prefix='kh-store-mutations-', dir=ROOT/'tmp') as directory:
        work = Path(directory); (work/'tmp').mkdir()
        shutil.copytree(ROOT/'core', work/'core', ignore=shutil.ignore_patterns('build', 'build-*'))
        copy = work/'core/src/known_hosts_store.c'; clean = copy.read_bytes(); build = work/'core/build'
        require(run(['cmake','-S',work/'core','-B',build,'-DCMAKE_BUILD_TYPE=Debug'],logs/'configure.log'))
        def compile_core():
            require(run(['cmake','--build',build,'--clean-first','--target','test_known_hosts_store','test_config'],logs/'build.log'))
        def target(label, reason=None):
            result = run([build/'test_known_hosts_store',work/'tmp'],logs/(label+'.log'),seconds=30)
            output = result.stdout + result.stderr
            if reason is None: require(result)
            elif result.returncode != 1 or 'FAIL store: '+reason not in output:
                raise RuntimeError('wrong mutation result: '+output)
        def control(label):
            require(run([build/'test_config',ROOT/'core/tests/fixtures'],logs/(label+'-control.log')))
        try:
            compile_core(); target('baseline'); control('baseline')
            for label, before, after, reason in cases:
                text = clean.decode()
                if text.count(before) != (2 if label == 'conflict' else 1): raise RuntimeError('anchor drift: '+label)
                text = text.replace(before,after)
                if label == 'recheck':
                    text = text.replace('length != latest_length || memcmp(current, latest, length)) {',
                                        'length != latest_length || memcmp(current, latest, length))) {')
                copy.write_text(text+'\n/* '+MARK+' */\n')
                compile_core(); target(label,reason); control(label)
                copy.write_bytes(clean)
                if hashlib.sha256(copy.read_bytes()).digest() != hashlib.sha256(clean).digest(): raise RuntimeError('restore SHA256')
                compile_core(); target(label+'-restored')
                print(f'PASS {label}: compiled target red / config green / restored green / SHA256',flush=True)
            if apple:
                package = work/'apple/GhostermCore'
                shutil.copytree(ROOT/'apple/GhostermCore',package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
                binding = package/'Sources/GhostermCore/GhostermCore.swift'; original = binding.read_bytes()
                env = dict(os.environ,TMPDIR=str(work/'tmp'))
                if Path('/Applications/Xcode.app/Contents/Developer').exists(): env.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
                command = ['xcrun','swift','test','--package-path',package,'--scratch-path',work/'swift-build']
                def swift_build(label):
                    require(run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',work/'swift-build'],logs/(label+'-build.log'),env))
                def swift_test(label, method, red=False):
                    result = run([*command,'--skip-build','--filter',method],logs/(label+'.log'),env,seconds=300)
                    output = result.stdout + result.stderr
                    if 'Executed 1 test' not in output: raise RuntimeError(output)
                    if not red: require(result)
                    elif result.returncode != 1 or 'XCTAssert' not in output: raise RuntimeError(output)
                target_method = 'KnownHostsSnapshotTests.testRemovalPreservesRevisionAndReportsConflict'
                control_method = 'GhostermCoreTests.testIdentitySourcesCopyValuesAcrossEdit'
                swift_build('swift-baseline'); swift_test('swift-baseline',target_method)
                for label,before,after in [('swift-commit','committed: result.committed','committed: false'),
                                          ('swift-conflict','conflict: result.conflict','conflict: false')]:
                    text=original.decode()
                    if text.count(before)!=1: raise RuntimeError('Swift anchor drift')
                    binding.write_text(text.replace(before,after)+'\n// '+MARK+'\n')
                    swift_build(label); swift_test(label,target_method,True); swift_test(label+'-control',control_method)
                    binding.write_bytes(original)
                    if binding.read_bytes()!=original: raise RuntimeError('Swift restore')
                    swift_build(label+'-restored'); swift_test(label+'-restored',target_method)
                    print(f'PASS {label}: compiled target red / config green / restored green / SHA256',flush=True)
        finally:
            copy.write_bytes(clean)
            if any(p.read_bytes()!=content for p,content in originals.items()): raise RuntimeError('production source changed')
    print(f'KNOWN HOSTS STORE MUTATIONS PASS: {len(cases)} C'+(' + 2 Swift' if apple else ''),flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(signum)
    for sig in (signal.SIGINT,signal.SIGTERM): signal.signal(sig,interrupted)
    os.environ.setdefault('TMPDIR',str(ROOT/'tmp'))
    main()
