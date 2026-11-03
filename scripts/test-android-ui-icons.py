#!/usr/bin/env python3
"""Isolated generator drift/rejection checks; never mutate production SVG/resources."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ui-icon-guard-',dir=ROOT/'tmp') as temporary:
    root=Path(temporary)
    for name in ('scripts/build-android-ui-icons.py','docs/design/android-ui-icons.svg','docs/design/android-prototype.html'):
        target=root/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/name,target)
    script=root/'scripts/build-android-ui-icons.py'
    def run(check=True):
        return subprocess.run(['python3',str(script)]+(['--check'] if check else []),cwd=root,capture_output=True,text=True)
    assert run(False).returncode==0
    assert run().returncode==0
    vector=root/'android/app/src/main/res/drawable/ic_gt_keyboard.xml'
    old=vector.read_bytes();vector.write_text(vector.read_text().replace('strokeWidth="1.7"','strokeWidth="3.0"'))
    result=run();assert result.returncode!=0 and 'UI icon drift' in result.stderr
    vector.write_bytes(old);assert run().returncode==0
    svg=root/'docs/design/android-ui-icons.svg';old=svg.read_bytes()
    svg.write_text(old.decode().replace('stroke-width="1.7"','stroke-width="2.0"',1))
    result=run();assert result.returncode!=0 and 'android-prototype.html' in result.stderr
    svg.write_bytes(old);assert run().returncode==0
    for name,before,after,expected in (
        ('duplicate','id="back"','id="arrow"','Duplicate SVG icon identifier'),
        ('stroke','stroke-linecap="round"','stroke-linecap="square"','Unsupported SVG stroke geometry'),
        ('transform','<path d=','<path transform="translate(1)" d=','Unsupported SVG path attributes'),
        ('shape','<path d="m9 5 7 7-7 7" />','<rect width="10" height="10" />','Unsupported SVG shape'),
    ):
        svg.write_text(old.decode().replace(before,after,1))
        result=run();assert result.returncode!=0 and expected in result.stderr,(name,result.stderr)
        svg.write_bytes(old);assert run().returncode==0
        print('PASS generator rejects '+name+'; restored check green')
    html=root/'docs/design/android-prototype.html';old=html.read_bytes()
    html.write_text(html.read_text().replace('--bg:#f7f8fc','--bg:#f8f8fc'))
    result=run();assert result.returncode!=0 and 'ui-reference.json' in result.stderr
    html.write_bytes(old);assert run().returncode==0
    print('PASS vector and palette drift rejected; restored check green')
print('ANDROID UI ICON GENERATOR GUARDS PASS: 7 groups')
