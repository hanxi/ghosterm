#!/usr/bin/env python3
"""换屏缩放：隔离 native 变异，目标红、字节网格对照绿、恢复绿与 SHA256。"""
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'apple/GhostermTerminal'
MARK = 'GT_BACKING_SCALE_MUTATION_MARK'
VIEW = 'Sources/GhostermTerminal/TerminalSurfaceView.swift'
SURFACE = 'Sources/GhostermTerminal/TerminalSurface.swift'
SHIM = 'Sources/CGhostty/shim.c'
SUITE = 'TerminalBackingScaleTests'
CASES = [
    ('引擎字体 DPI', SHIM,
     '  ghostty_surface_set_content_scale(s->surface, scale, scale);',
     '  (void)scale;', 'testBackingScaleChangeAndSubsequentWindowResize'),
    ('Metal 合成比例', SURFACE,
     '        view.layer?.contentsScale = scale',
     '        _ = scale', 'testBackingScaleChangeAndSubsequentWindowResize'),
    ('backing 回调', VIEW,
     '        super.viewDidChangeBackingProperties()\n        syncSurfaceGeometry()',
     '        super.viewDidChangeBackingProperties()', 'testBackingScaleChangeAndSubsequentWindowResize'),
    ('换屏通知', VIEW,
     '            DispatchQueue.main.async { [weak self] in self?.syncSurfaceGeometry() }',
     '            _ = self', 'testScreenChangeRefreshesBackingPropertiesWithoutLayout'),
    ('换屏像素尺寸', VIEW,
     'width: UInt32(bounds.width * scale)',
     'width: UInt32(bounds.width * 2)', 'testBackingScaleChangeAndSubsequentWindowResize'),
    ('未上屏窗口也同步', VIEW,
     'guard let window, bounds.width > 0',
     'guard let window, window.isVisible, bounds.width > 0',
     'testMovingHostIntoAnotherWindowUsesItsScaleImmediately'),
]


def main():
    originals = {name: (PACKAGE / name).read_bytes() for name in {c[1] for c in CASES}}
    hashes = {name: hashlib.sha256(data).digest() for name, data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('残留变异标记，拒绝起跑')
    (ROOT / 'tmp').mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='terminal-backing-scale-', dir=ROOT / 'tmp'))
    package = work / 'apple/GhostermTerminal'
    shutil.copytree(PACKAGE, package, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
    for name in ('build', 'core'):
        (work / name).symlink_to(ROOT / name, target_is_directory=True)
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'))
    env.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')

    def interrupted(sig, frame):
        raise RuntimeError(f'换屏变异被中断：{sig}')

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)

    def restore():
        for name, data in originals.items():
            (package / name).write_bytes(data)
            for path in (package / name, PACKAGE / name):
                if hashlib.sha256(path.read_bytes()).digest() != hashes[name]:
                    raise RuntimeError(f'SHA256 复原不一致：{path}')

    def probe(label, test, red=False):
        result = subprocess.run([
            'bash', '-c',
            '. "$1"; shift; gt_timeout_require backing-scale; gt_timeout 180 "$@"',
            'backing-scale', str(ROOT / 'scripts/lib/gt-timeout.sh'),
            'swift', 'test', '--package-path', str(package), '--filter', test,
        ], env=env, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        out = result.stdout
        (work / (label + '.log')).write_text(out)
        valid = ('Build complete!' in out and 'Executed ' in out
                 and 'Executed 0 tests' not in out and 'skipped' not in out
                 and '0 unexpected' in out and 'unexpected signal' not in out)
        valid = valid and (result.returncode == 1 and 'XCTAssert' in out if red else
                           result.returncode == 0 and 'with 0 failures' in out)
        if not valid:
            print(out, flush=True)
            raise RuntimeError(f'目标红/对照绿判据不成立：{work / (label + ".log")}')

    control = 'TerminalSurfaceTests/testOutputReachesGhosttyGrid'
    try:
        probe('baseline', SUITE)
        probe('baseline-control', control)
        for index, (label, name, old, new, test) in enumerate(CASES):
            source = originals[name].decode()
            if source.count(old) != 1:
                raise RuntimeError(f'变异锚点不唯一：{label}')
            try:
                (package / name).write_text(source.replace(old, new) + '\n// ' + MARK + '\n')
                probe(f'{index}-red', SUITE + '/' + test, red=True)
                probe(f'{index}-control', control)
            finally:
                restore()
            probe(f'{index}-restored', SUITE + '/' + test)
            print(f'PASS {label}：目标红、同变异对照绿、恢复绿与 SHA256', flush=True)
        probe('restored', SUITE)
        print(f'PASS {len(CASES)} 组换屏缩放变异；日志：{work}', flush=True)
    finally:
        restore()


if __name__ == '__main__':
    main()
