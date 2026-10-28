#!/usr/bin/env python3
"""Real read-only DMG teardown, with isolated mutations of the shared helper."""
import hashlib, os, pathlib, shutil, signal, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'scripts/lib/package-dmg.sh'
MARK = 'GT_PACKAGE_DMG_MUTATION_MARK'

def run(args, env=None):
    result = subprocess.run(['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require package-dmg; shift; gt_timeout 60 "$@"', 'package-dmg', str(ROOT)] + list(map(str, args)), capture_output=True, text=True, env=env)
    return result.returncode, result.stdout + result.stderr

def main():
    if not shutil.which('hdiutil'): raise RuntimeError('real macOS hdiutil required; no SKIP')
    original = SOURCE.read_bytes(); digest = hashlib.sha256(original).digest()
    if MARK.encode() in original: raise RuntimeError('leftover mutation marker: exit 2')
    def interrupt(sig, frame): raise InterruptedError(sig)
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupt)
    with tempfile.TemporaryDirectory(prefix='package-dmg-', dir=ROOT/'tmp') as directory:
        folder = pathlib.Path(directory); stage = folder/'stage'; stage.mkdir()
        (stage/'sentinel').write_text('owned read-only volume\n')
        image = folder/'probe.dmg'; helper = folder/'package-dmg.sh'; helper.write_bytes(original)
        tools = folder/'bin'; tools.mkdir(); events = folder/'events'
        wrapper = tools/'hdiutil'
        wrapper.write_text('''#!/bin/bash
printf '%s\\n' "$*" >> "$GT_DMG_EVENTS"
case "$GT_DMG_FAULT" in
busy) case " $* " in *" -force "*) exec /usr/bin/hdiutil "$@";; *) exit 1;; esac;;
refuse) exit 1;;
lie) exit 0;;
esac
exec /usr/bin/hdiutil "$@"
'''); wrapper.chmod(0o700)
        env = dict(os.environ, TMPDIR=str(ROOT/'tmp'), PATH=str(tools)+os.pathsep+os.environ['PATH'], GT_DMG_EVENTS=str(events))
        def command(args):
            code, out = run(args, env)
            if code: print(out, flush=True); raise RuntimeError('DMG fixture/tool failed')
        command(['/usr/bin/hdiutil', 'create', '-volname', 'GhostermCleanupProbe', '-srcfolder', stage, '-ov', '-format', 'UDZO', image])
        work = folder/'work'; mount = work/'mnt'
        def cleanup_mount():
            if os.path.ismount(mount): command(['/usr/bin/hdiutil','detach','-force',mount])
            if os.path.ismount(mount): raise RuntimeError('owned test DMG still mounted; refusing recursive deletion')
        def check(mode):
            work.mkdir(); mount.mkdir(); (work/'keep').write_text('do not delete while mounted')
            events.write_text('')
            command(['/usr/bin/hdiutil','attach',image,'-nobrowse','-readonly','-mountpoint',mount])
            assert os.path.ismount(mount), 'real mount precondition'
            try:
                code, out = run(['bash','-c','. "$1"; gt_package_cleanup_work "$2" "$3" 0','dmg-check',helper,work,mount], dict(env,GT_DMG_FAULT=mode))
                if mode == 'busy':
                    valid = code == 0 and not work.exists() and not os.path.ismount(mount) and '-force' in events.read_text()
                else:
                    valid = code == 1 and os.path.ismount(mount) and (work/'keep').is_file() and '保留临时目录' in out and 'rm:' not in out
                if not valid: print(out, flush=True)
                assert valid, 'DMG behavior assertion: '+mode
            finally:
                cleanup_mount()
                if work.exists(): shutil.rmtree(work)
        def control():
            work.mkdir(); (work/'keep').write_text('ordinary unmounted cleanup')
            command(['bash','-c','. "$1"; gt_package_cleanup_work "$2" "" 0','dmg-control',helper,work])
            assert not work.exists(), 'unmounted independent control'
        for mode in ['busy','refuse','lie']: check(mode)
        control()
        cases = [
            ('force', 'hdiutil detach -force "$mount" >/dev/null 2>&1 || return 1', 'false || return 1', 'busy'),
            ('mounted-guard', 'if ! gt_package_detach_dmg "$mount"; then', 'if false; then', 'refuse'),
            ('verify', '  gt_package_is_unmounted "$mount"\n}', '  return 0\n}', 'lie'),
        ]
        try:
            for name, old, new, target in cases:
                text = original.decode(); assert text.count(old) == 1, name
                helper.write_text(text.replace(old,new)+'\n# '+MARK+'\n')
                red = False
                try: check(target)
                except AssertionError as error:
                    if 'DMG behavior assertion' not in str(error): raise
                    red = True
                assert red, 'mutation did not reach its behavior assertion: '+name
                control()
                helper.write_bytes(original); assert hashlib.sha256(helper.read_bytes()).digest() == digest
                check(target)
                print('PASS package DMG '+name+': target red, control green, SHA rollback',flush=True)
        finally:
            helper.write_bytes(original); cleanup_mount()
            assert hashlib.sha256(SOURCE.read_bytes()).digest() == digest
    print('PASS package DMG mutations (3 groups)')
if __name__ == '__main__': main()
