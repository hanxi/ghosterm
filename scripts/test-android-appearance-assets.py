#!/usr/bin/env python3
"""Isolated drift guards for pinned theme/font inputs and their generated consumers."""
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='appearance-assets-',dir=ROOT/'tmp') as temporary:
    root=Path(temporary)
    paths=['scripts/build-android-appearance.py','docs/design/android-terminal-themes.json','terminal/android/THEMES.json','terminal/android/FONTS.json','vendor/ghostty/build.zig.zon']
    paths += [str(p.relative_to(ROOT)) for p in (ROOT/'terminal/android/src/main/assets/licenses').glob('*')]
    paths += ['terminal/android/src/main/res/font/jetbrains_mono.ttf','terminal/android/src/main/res/font/source_code_pro.ttf']
    for name in paths:
        target=root/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/name,target)
    for name in ['android/domain/src/main/kotlin/com/hanxi/ghosterm/domain','android/app/src/androidTest/assets','docs/design']:(root/name).mkdir(parents=True,exist_ok=True)
    def run(check=True):return subprocess.run(['python3',str(root/'scripts/build-android-appearance.py')]+(['--check'] if check else []),capture_output=True,text=True)
    assert run(False).returncode==0
    assert run().returncode==0
    targets=['docs/design/android-terminal-themes.json','vendor/ghostty/build.zig.zon','terminal/android/src/main/assets/licenses/TerminalThemes-MIT.txt','terminal/android/src/main/res/font/jetbrains_mono.ttf','terminal/android/src/main/res/font/source_code_pro.ttf','terminal/android/src/main/assets/licenses/SourceCodePro-OFL.txt','android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/TerminalThemeCatalog.kt','docs/design/android-appearance-catalog.js','android/app/src/androidTest/assets/terminal-themes-reference.json']
    for name in targets:
        path=root/name;old=path.read_bytes()
        path.write_bytes(old.replace(b'N-V-__8AADNBBQDD4MFOa3Y7R_A8PHIeWQ1IZdOiBFZS3nZ7',b'bad-pin') if name.endswith('.zon') else old+b' ')
        assert run().returncode!=0,name
        path.write_bytes(old);assert run().returncode==0,name
        print('PASS drift rejected / restored green: '+name)
print('ANDROID APPEARANCE ASSET GUARDS PASS: 9 groups')
