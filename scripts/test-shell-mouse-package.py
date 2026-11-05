#!/usr/bin/env python3
"""Exercise the real packaged zsh loader and mouse cleanup, including failures."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    args = parser.parse_args()
    app = args.app.resolve()
    work = Path(tempfile.mkdtemp(prefix='mouse-package-', dir=ROOT / 'tmp'))
    resources = Path('Contents/Resources/GhostermApp_GhostermAppModel.bundle/ShellIntegration')
    originals = {str(path.relative_to(app)): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in (app / resources).rglob('*') if path.is_file()}
    assert len(originals) == 2, 'Packaged shell startup resources missing'
    config = work / 'control.conf'
    config.write_text('font-size = 14\nbackground = #123456\n')
    timed = runpy.run_path(str(ROOT / 'scripts/test-shell-mouse-recovery.py'))['timed']

    def run(label, target, mouse=True, green=True):
        home = work / (label + '-home'); home.mkdir()
        arguments = ['--mouse-recovery-check', str(home)] if mouse else ['--config-check', str(config)]
        result = timed([target / 'Contents/MacOS/GhostermApp', *arguments], limit=60,
                       env=dict(os.environ, TMPDIR=str(ROOT / 'tmp')), cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (work / (label + '.log')).write_text(result.stdout)
        good = (result.returncode == 0 and ('MOUSE_RECOVERY_OK' if mouse else 'CONFIG_OK diagnostics=0') in result.stdout) if green else (result.returncode == 1 and '鼠标恢复校验失败：' in result.stdout)
        if not good:
            print(result.stdout, flush=True)
            raise RuntimeError('Packaged mouse recovery check failed: ' + label)

    run('baseline', app)
    run('baseline-control', app, mouse=False)
    for case in ('resources-missing', 'bootstrap-missing', 'hook-not-loaded', 'reset-missing'):
        target = work / (case + '.app')
        shutil.copytree(app, target, symlinks=True)
        directory = target / resources
        if case == 'resources-missing':
            shutil.rmtree(directory)
        elif case == 'bootstrap-missing':
            (directory / 'zsh/.zshenv').unlink()
        elif case == 'hook-not-loaded':
            path = directory / 'zsh/.zshenv'
            data = path.read_text()
            anchor = 'builtin source "$__ghosterm_mouse_root/mouse-recovery.zsh"'
            assert data.count(anchor) == 1
            path.write_text(data.replace(anchor, ':'))
        else:
            path = directory / 'mouse-recovery.zsh'
            data = path.read_text()
            anchor = "        builtin printf '\\033[?9l\\033[?1000l\\033[?1002l\\033[?1003l\\033[?1005l\\033[?1006l\\033[?1015l\\033[?1016l'"
            assert data.count(anchor) == 1
            path.write_text(data.replace(anchor, '        :'))
        run(case + '-target', target, green=False)
        run(case + '-control', target, mouse=False)
        shutil.rmtree(target)
        shutil.copytree(app, target, symlinks=True)
        run(case + '-restored', target)
        actual = {str(path.relative_to(target)): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in (target / resources).rglob('*') if path.is_file()}
        assert actual == originals, 'Packaged resources SHA256 restore failed'
        shutil.rmtree(target)
        print('PASS packaged mouse recovery', case, 'normal check red / same-app config green / restore green / SHA256', flush=True)
    print('PASS packaged mouse recovery; evidence', work)


if __name__ == '__main__':
    main()
