#!/usr/bin/env python3
"""macOS owned subprocess deaths and explicit native plugin removal recovery.

Uses a copied Apple package, a default Lua-off Core and declaration-only fixtures.
Injected SIGKILLs are separate observations, never assertion-red evidence.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
TMP = ROOT / 'tmp'
PROBE = 'GhostermAppTests.PluginRemovalProcessProbeTests/testRunExplicitOwnedRequest'
CONTROL = 'GhostermAppTests.HostListTests/testReadingDoesNotMutateConfig'
MODULE = 'apple/GhostermApp/Sources/GhostermAppModel/PluginReleaseRemoval.swift'
CONTROLLER = 'apple/GhostermApp/Sources/GhostermAppModel/ExtensionController.swift'
PLUGIN = 'org.ghosterm.removal-probe'
JOURNAL = 'removal-v1.jsonl'
CASES = ['before-intent', 'after-intent', 'after-detach', 'after-unlink', 'before-clear',
         'changed-file', 'unknown-member', 'new-slot', 'enabled', 'root-swap', 'bad-record',
         'shared-lease', 'preview-lease']
RECIPES = [
    ('absence', MODULE, 'let absent = members.count - current.count', 'let absent = 0', 'after-unlink'),
    ('foreign-file', MODULE, 'item.directory ? item.stamp.sameObject(old.stamp) : item == old',
     'item.stamp.sameObject(old.stamp)', 'changed-file'),
    ('disabled', MODULE, '!choice.choices.values.contains(where: { $0.releaseID == header.releaseID && $0.enabled })',
     'choice.choices.count <= 64', 'enabled'),
    ('root-identity', MODULE,
     'guard header.root.sameObject(try statFD(root)) else { throw Failure.changed }\n        try verifyStage(root, header: header)\n        try requireDisabled(owner, header: header, checkRevision: false)',
     'guard root >= 0 else { throw Failure.changed }\n        try verifyStage(root, header: header)\n        try requireDisabled(owner, header: header, checkRevision: false)', 'root-swap'),
    ('new-slot', MODULE, 'name = header.tombstone', 'name = header.releaseID', 'new-slot'),
    ('no-replay', CONTROLLER,
     'func installedVersions() async throws -> InstalledCatalog {\n            try await perform {\n                try self.checkOpen()',
     'func installedVersions() async throws -> InstalledCatalog {\n            try await perform {\n                try self.checkOpen()\n                if let replay = try self.catalogOwner(exclusive: true), try replay.hasPendingRemoval() { _ = try PluginReleaseRemoval.resume(owner: replay) }', 'after-detach'),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def scratch_root():
    requested = Path(os.environ.get('TMPDIR', TMP)).resolve()
    return requested if requested.is_dir() and requested.is_relative_to(TMP.resolve()) else TMP


def environment():
    # xctest prints its environment for malformed invocations. Do not propagate
    # the caller's credentials, terminal integration, SSH or model settings.
    scratch = str(scratch_root())
    return dict(PATH=os.environ.get('PATH', '/usr/bin:/bin'), TMPDIR=scratch, TMP=scratch, TEMP=scratch,
                DEVELOPER_DIR=os.environ.get('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer'),
                LC_ALL='C.UTF-8', GHOSTERM_AUTOMATION_OFFSCREEN='1')


def thaw(path):
    if path.is_symlink() or not path.is_dir():
        return
    path.chmod(0o700)
    for directory, dirs, _ in os.walk(path, followlinks=False):
        for name in dirs:
            child = Path(directory) / name
            if not child.is_symlink(): child.chmod(0o700)
        dirs[:] = [name for name in dirs if not (Path(directory) / name).is_symlink()]


@contextmanager
def private_work(prefix):
    work = Path(tempfile.mkdtemp(prefix=prefix, dir=scratch_root()))
    try:
        yield work
    finally:
        thaw(work)
        shutil.rmtree(work)


def snapshot(root):
    result = {}
    for directory, dirs, names in os.walk(root, followlinks=False):
        for path in [Path(directory), *(Path(directory) / name for name in names)]:
            value = path.lstat()
            assert stat.S_ISDIR(value.st_mode) or stat.S_ISREG(value.st_mode), path
            result[str(path.relative_to(root))] = [value.st_dev, value.st_ino, value.st_mode,
                value.st_mtime_ns, value.st_ctime_ns, sha(path) if path.is_file() else None]
        assert not any((Path(directory) / name).is_symlink() for name in dirs)
    return result


class Fixture:
    def __init__(self, work, label):
        self.scope = work / (label + '-' + uuid.uuid4().hex[:8]); self.scope.mkdir(mode=0o700)
        self.root = self.scope / 'installed'; self.root.mkdir(mode=0o700)
        self.nonce = str(uuid.uuid4())
        # Inventory reads unauthenticated declarations. These deliberately have
        # no valid release signature and are never installed, approved or run.
        self.descriptor = json.dumps(dict(pluginId=PLUGIN, pluginVersion='0.1.0',
            publisher='Untrusted subprocess fixture', packageSHA256='0' * 64), sort_keys=True).encode()
        self.release = hashlib.sha256(self.descriptor).hexdigest()
        self.populate(self.root / self.release)
        self.selection(False)
        self.total = len(snapshot(self.root / self.release))

    def populate(self, directory, replacement=False):
        (directory / 'payload/nested').mkdir(parents=True, mode=0o700)
        contents = {'release.json': self.descriptor, 'release.sig': bytes(64), 'policy.json': b'{}',
                    'payload/nested/data.txt': b'replacement nested' if replacement else b'first',
                    'payload/keep.txt': b'replacement keep' if replacement else b'second'}
        for name, data in contents.items():
            path = directory / name
            with path.open('wb') as output:
                output.write(data); output.flush(); os.fsync(output.fileno())
            path.chmod(0o400)
        for path in [directory / 'payload/nested', directory / 'payload', directory]: path.chmod(0o500)

    def selection(self, enabled):
        raw = json.dumps(dict(schemaVersion=1, nonce=str(uuid.uuid4()), choices={PLUGIN: dict(
            releaseID=self.release, enabled=enabled, generation=str(uuid.uuid4()))}), sort_keys=True).encode()
        path = self.root / 'selection-v1.json'; path.write_bytes(raw); path.chmod(0o600)
        self.revision = hashlib.sha256(raw).hexdigest()

    def members(self, directory=None):
        directory = directory or self.root
        return {str(p.relative_to(self.root)): sha(p) for p in directory.rglob('*') if stat.S_ISREG(p.lstat().st_mode) and p.name != JOURNAL}

    def detached(self):
        header = json.loads((self.root / JOURNAL).read_bytes().splitlines()[0])
        return self.root / header['tombstone']

    def request(self, action, **kwargs):
        value = self.root.stat()
        body = dict(nonce=self.nonce, scope=str(self.scope), device=str(value.st_dev), inode=str(value.st_ino),
                    releaseID=self.release, action=action, revision=self.revision, pending=False, versions=0, survivors={})
        body.update(kwargs)
        request = self.scope / 'request.json'
        request.write_text(json.dumps(body)); request.chmod(0o600)
        return request


class Driver:
    def __init__(self, bundle, xctest, logs):
        self.bundle, self.xctest, self.logs = bundle, xctest, logs
        self.commands = []

    @contextmanager
    def child(self, selected, fixture=None, **kwargs):
        env = environment()
        if fixture:
            env.update(GHOSTERM_REMOVAL_PROBE_REQUEST=str(fixture.request(**kwargs)),
                       GHOSTERM_REMOVAL_PROBE_NONCE=fixture.nonce)
        child = subprocess.Popen([self.xctest, '-XCTest', selected, self.bundle], cwd=ROOT, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            yield child
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGTERM)
                try: child.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL); child.communicate(timeout=5)

    def ready(self, child, fixture, point, action='begin'):
        deadline, output, line_buffer = time.monotonic() + 30, bytearray(), bytearray()
        while time.monotonic() < deadline:
            assert child.poll() is None, 'owner exited before the requested checkpoint'
            if not select.select([child.stdout], [], [], min(1, max(0, deadline - time.monotonic())))[0]: continue
            data = os.read(child.stdout.fileno(), 8192)
            assert data, 'owner output ended before readiness'
            output.extend(data); line_buffer.extend(data)
            assert len(output) <= 1048576, 'owner diagnostic budget'
            while b'\n' in line_buffer:
                line, _, remainder = line_buffer.partition(b'\n'); line_buffer = bytearray(remainder)
                if not line.startswith(b'GHOSTERM_REMOVAL_READY '): continue
                proof = json.loads(line.removeprefix(b'GHOSTERM_REMOVAL_READY '))
                value = fixture.root.stat()
                assert proof == dict(nonce=fixture.nonce, pid=child.pid, point=point,
                    action=action, device=str(value.st_dev), inode=str(value.st_ino)), proof
                assert child.poll() is None, 'ready owner must still be alive'
                return bytes(output), proof
        raise AssertionError('owner readiness timeout')

    def crash(self, fixture, point, label, action='begin'):
        with self.child(PROBE, fixture, action=action, point=point) as child:
            prefix, proof = self.ready(child, fixture, point, action)
            child.send_signal(signal.SIGKILL)
            suffix, _ = child.communicate(timeout=10)
            assert child.returncode == -signal.SIGKILL, (label, child.returncode)
        raw = prefix + suffix
        (self.logs / (label + '-death.log')).write_bytes(raw)
        self.commands.append(dict(label=label, kind='injected-process-death', signal='SIGKILL',
            status=child.returncode, checkpoint=proof, logSHA256=hashlib.sha256(raw).hexdigest()))
        return child.pid

    def run(self, label, fixture=None, *, selected=PROBE, red=False, **kwargs):
        with self.child(selected, fixture, **kwargs) as child:
            raw, _ = child.communicate(timeout=60)
        log = raw.decode(errors='replace')
        (self.logs / (label + '.log')).write_bytes(raw)
        assert not any(value in log for value in ['AddressSanitizer', 'UndefinedBehaviorSanitizer', 'unexpected signal', 'Fatal error:']), (label, log[-6000:])
        if red:
            assert child.returncode == 1 and re.search(r'Executed 1 test, with [1-9][0-9]* failures? \(0 unexpected\)', log), (label, log[-6000:])
            assert re.search(r': error: .* : (?:XCTAssert\w+ failed|failed -)', log), (label, log[-6000:])
        else:
            assert child.returncode == 0 and 'Executed 1 test, with 0 failures (0 unexpected)' in log and 'skipped' not in log.lower(), (label, log[-6000:])
        records = [json.loads(line.removeprefix('GHOSTERM_REMOVAL_RESULT ')) for line in log.splitlines() if line.startswith('GHOSTERM_REMOVAL_RESULT ')]
        if fixture:
            assert len(records) == 1 and records[0]['nonce'] == fixture.nonce and records[0]['pid'] == child.pid, (label, records)
        self.commands.append(dict(label=label, kind='normal-assertion-red' if red else 'normal-test-green', status=child.returncode,
            pid=child.pid, result=records, logSHA256=hashlib.sha256(raw).hexdigest()))
        return records[0] if records else None

    def case(self, work, name, *, label=None, red=False):
        label = label or name; fixture = Fixture(work, label)
        point = name if name in CASES[:5] else 'after-detach'
        if name in ['shared-lease', 'preview-lease']:
            action = 'hold-shared' if name == 'shared-lease' else 'hold-preview'
            with self.child(PROBE, fixture, action=action, point='holding') as child:
                prefix, proof = self.ready(child, fixture, 'holding', action)
                self.run(label + '-busy', fixture, action='preview-cancel', expectedError='removal.occupied')
                child.send_signal(signal.SIGKILL); suffix, _ = child.communicate(timeout=10)
                assert child.returncode == -signal.SIGKILL
                raw = prefix + suffix
                (self.logs / (label + '-death.log')).write_bytes(raw)
                self.commands.append(dict(label=label, kind='injected-process-death', status=child.returncode,
                    signal='SIGKILL', checkpoint=proof, logSHA256=hashlib.sha256(raw).hexdigest()))
            before = snapshot(fixture.root)
            self.run(label + '-released', fixture, action='preview-cancel')
            assert snapshot(fixture.root) == before
            print('PASS ' + label, flush=True)
            return
        self.crash(fixture, point, label)
        intent = fixture.root / JOURNAL
        pending = point != 'before-intent'
        before = snapshot(fixture.root)
        journal_sha = sha(intent) if pending else None
        self.run(label + '-inspect', fixture, action='inspect', pending=pending,
            versions=1 if point in ['before-intent', 'after-intent'] else 0,
            journalSHA=journal_sha, survivors=fixture.members(), red=red and name == 'after-detach')
        if red and name == 'after-detach':
            print('PASS ' + label + ' assertion', flush=True); return
        assert snapshot(fixture.root) == before, 'new process inspection must not replay deletion'
        action = 'confirm' if not pending else 'resume'
        kwargs = dict(removed=fixture.total - (1 if point == 'after-unlink' else 0), absent=1 if point == 'after-unlink' else 0)
        if point == 'before-clear': kwargs.update(removed=0, absent=fixture.total)
        preserved_slot = None
        if name == 'changed-file':
            file = fixture.detached() / 'payload/keep.txt'; file.chmod(0o600); file.write_bytes(b'foreign'); file.chmod(0o400)
            kwargs = dict(expectedError='removal.changed', survivors=fixture.members())
        elif name == 'unknown-member':
            parent = fixture.detached() / 'payload'; parent.chmod(0o700)
            file = parent / 'foreign'; file.write_bytes(b'unknown member'); file.chmod(0o400); parent.chmod(0o500)
            kwargs = dict(expectedError='removal.changed', survivors=fixture.members())
        elif name == 'new-slot':
            directory = fixture.root / fixture.release; fixture.populate(directory, replacement=True)
            preserved_slot = snapshot(directory)
            kwargs.update(survivors=fixture.members(directory), regressionErrors=['removal.changed'])
        elif name == 'enabled':
            fixture.selection(True)
            kwargs = dict(expectedError='removal.enabled', survivors=fixture.members())
        elif name == 'root-swap':
            old = fixture.scope / 'old-installed'; fixture.root.rename(old); fixture.root.mkdir(mode=0o700)
            for path in old.iterdir():
                mode = stat.S_IMODE(path.stat().st_mode)
                if path.is_dir(): path.chmod(0o700)  # macOS updates '..' on a cross-parent directory move.
                moved = path.rename(fixture.root / path.name)
                moved.chmod(mode)
            action = 'resume-native'; kwargs = dict(expectedError='removal.changed', survivors=fixture.members())
        elif name == 'bad-record':
            intent.write_bytes(b'{\n')
            kwargs = dict(expectedError='json.json', survivors=fixture.members())
        before = snapshot(fixture.root)
        self.run(label + '-action', fixture, action=action, red=red, **kwargs)
        if not red:
            if 'expectedError' in kwargs:
                assert snapshot(fixture.root) == before, 'rejected maintenance must preserve the exact tree'
            else:
                if preserved_slot is not None: assert snapshot(fixture.root / fixture.release) == preserved_slot
                self.run(label + '-final', fixture, action='inspect', pending=False, versions=1 if preserved_slot is not None else 0)
        print('PASS ' + label + (' assertion' if red else ''), flush=True)


def build(package, work, label, *, baseline=False):
    selected = 'PluginReleaseRemovalTests|HostListTests.testReadingDoesNotMutateConfig' if baseline else 'HostListTests.testReadingDoesNotMutateConfig'
    child = subprocess.Popen(['swift', 'test', '--package-path', str(package), '--filter', selected], cwd=ROOT,
        env=environment(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        output, _ = child.communicate(timeout=300)
    finally:
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGTERM)
            try: child.communicate(timeout=5)
            except subprocess.TimeoutExpired: os.killpg(child.pid, signal.SIGKILL); child.communicate(timeout=5)
    (work / (label + '.log')).write_bytes(output)
    count = 23 if baseline else 1
    assert child.returncode == 0 and f'Executed {count} test' + ('s' if count > 1 else '') + ', with 0 failures' in output.decode(), output.decode()[-8000:]
    assert 'skipped' not in output.decode().lower()
    return hashlib.sha256(output).hexdigest()


def interruption_worker(bundle):
    bundle = bundle.resolve(strict=True)
    assert TMP in bundle.parents and bundle.name == 'GhostermAppTests.xctest'
    xctest = Path(subprocess.check_output(['/usr/bin/xcrun', '--find', 'xctest'], env=environment(), text=True).strip())
    with private_work('rc-interrupt-') as work:
        fixture = Fixture(work, 'owned')
        driver = Driver(bundle, xctest, work)
        with driver.child(PROBE, fixture, action='hold-shared', point='holding') as child:
            _, proof = driver.ready(child, fixture, 'holding', 'hold-shared')
            print('GHOSTERM_DRIVER_READY ' + json.dumps(dict(pid=os.getpid(), child=child.pid, root=str(work), proof=proof)), flush=True)
            signal.pause()
            raise AssertionError('worker must be interrupted explicitly')


def check_interruptions(bundle, work):
    records = []
    for number in [signal.SIGINT, signal.SIGTERM]:
        child = subprocess.Popen([sys.executable, '-B', ROOT / 'scripts/test-plugin-removal-crash.py', '--interrupt-worker', bundle],
            cwd=ROOT, env=environment(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        output, buffered, proof = bytearray(), bytearray(), None
        try:
            deadline = time.monotonic() + 30
            while proof is None and time.monotonic() < deadline:
                assert child.poll() is None, 'interruption worker exited before readiness'
                if not select.select([child.stdout], [], [], 1)[0]: continue
                data = os.read(child.stdout.fileno(), 8192); assert data
                output.extend(data); buffered.extend(data)
                assert len(output) <= 1048576
                while b'\n' in buffered:
                    line, _, rest = buffered.partition(b'\n'); buffered = bytearray(rest)
                    if line.startswith(b'GHOSTERM_DRIVER_READY '): proof = json.loads(line.removeprefix(b'GHOSTERM_DRIVER_READY '))
            assert proof is not None and proof['pid'] == child.pid
            assert child.poll() is None
            owned = Path(proof['root']); assert owned.parent == scratch_root() and owned.is_dir()
            os.kill(proof['child'], 0)  # Verified live native lease holder, not just a marker file.
            child.send_signal(number)
            suffix, _ = child.communicate(timeout=15); output.extend(suffix)
            assert child.returncode == 128 + number, child.returncode
            assert not owned.exists(), 'driver interruption must clean its own private tree'
            try: os.kill(proof['child'], 0)
            except ProcessLookupError: pass
            else: raise AssertionError('native owner survived driver interruption')
            records.append(dict(signal=signal.Signals(number).name, status=child.returncode, ready=proof,
                logSHA256=hashlib.sha256(output).hexdigest()))
            (work / ('driver-' + signal.Signals(number).name + '.log')).write_bytes(output)
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGTERM)
                try: child.communicate(timeout=5)
                except subprocess.TimeoutExpired: os.killpg(child.pid, signal.SIGKILL); child.communicate(timeout=5)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--case', choices=CASES, action='append')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--interrupt-worker', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    TMP.mkdir(exist_ok=True)
    if args.interrupt_worker:
        interruption_worker(args.interrupt_worker); return
    if not args.out: parser.error('--out is required')
    out = args.out.resolve(); assert TMP in out.parents
    assert 'GHOSTERM_WITH_PLUGIN_LUA:BOOL=OFF' in (ROOT / 'core/build/CMakeCache.txt').read_text(), 'requires default Lua-off Core'
    inputs = {str(p.relative_to(ROOT)): sha(p) for directory in ['apple', 'core/include', 'core/src', 'core/tests', 'packages', 'scripts']
        for p in (ROOT / directory).rglob('*') if p.is_file() and not any(part in p.parts for part in ['.build', '.swiftpm', '__pycache__'])}
    xctest = Path(subprocess.check_output(['/usr/bin/xcrun', '--find', 'xctest'], env=environment(), text=True).strip())
    binaries = {str(p): sha(p) for p in [xctest, ROOT / 'core/build/libghosterm_core.a', ROOT / 'build/host-native/lib/libssh2.a', ROOT / 'build/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a']}
    groups = []
    with private_work('rc-') as work:
        source = work / 'source'
        for name in ['GhostermApp', 'GhostermCore', 'GhostermTerminal']:
            shutil.copytree(ROOT / 'apple' / name, source / 'apple' / name, symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        (source / 'core').mkdir()
        for name in ['build', 'include']: (source / 'core' / name).symlink_to(ROOT / 'core' / name, target_is_directory=True)
        for name in ['build', 'patches']: (source / name).symlink_to(ROOT / name, target_is_directory=True)
        (source / 'tmp').mkdir()
        package = source / 'apple/GhostermApp'
        baseline_sha = build(package, work, 'baseline', baseline=True)
        bundle = next((package / '.build').rglob('GhostermAppTests.xctest'))
        driver = Driver(bundle, xctest, work)
        for name in args.case or CASES: driver.case(work, name)
        interruptions = check_interruptions(bundle, work)
        for label, filename, old, new, name in RECIPES if args.mutate else []:
            original = (ROOT / filename).read_bytes(); assert original.count(old.encode()) == 1, label
            copied = source / filename; stem = Path(filename).stem
            objects = {p: sha(p) for p in (package / '.build').rglob('*') if p.name in [stem + '.swift.o', stem + '.o']}
            assert objects, label
            copied.write_bytes(original.replace(old.encode(), new.encode()))
            try:
                build(package, work, label + '-compile')
                assert any(sha(path) != digest for path, digest in objects.items()), 'changed source must compile into the actual production object'
                driver.case(work, name, label=label + '-red', red=True)
                driver.run(label + '-control', selected=CONTROL)
            finally:
                copied.write_bytes(original)
            build(package, work, label + '-restore-compile')
            driver.case(work, name, label=label + '-restored')
            assert sha(copied) == inputs[filename]
            groups.append(label)
        binary = bundle / 'Contents/MacOS/GhostermAppTests'
        result = dict(schemaVersion=1, scope='macOS declaration-only owned SIGKILL / cross-process leases and explicit removal recovery; not power loss, install or signed Runtime',
            baselineTests=23, cases=args.case or CASES, mutations=groups, commands=driver.commands,
            driverInterruptions=interruptions, inputs=inputs, binaries=binaries, xctestSHA256=sha(binary), baselineLogSHA256=baseline_sha)
    assert all(sha(ROOT / name) == digest for name, digest in inputs.items()), 'original sources changed'
    assert all(sha(Path(name)) == digest for name, digest in binaries.items()), 'fixed archives/tools changed'
    out.parent.mkdir(parents=True, exist_ok=True); out.write_text(json.dumps(result, indent=2) + '\n')
    print(f'REMOVAL CRASH PASS: {len(args.case or CASES)} cases, {len(groups)} mutation/control/restore groups; {out}', flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise SystemExit(128 + number)
    for number in [signal.SIGINT, signal.SIGTERM]: signal.signal(number, interrupted)
    main()
