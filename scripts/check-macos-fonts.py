#!/usr/bin/env python3
"""Verify Source Code Pro's pinned assets and the staged SwiftPM font resource bundle."""
import argparse
import hashlib
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'apple/GhostermApp/Sources/GhostermAppModel/Resources/Fonts'
def verify(directory):
    manifest=json.loads((directory/'manifest.json').read_text())
    expected=json.loads((SOURCE/'manifest.json').read_text())
    if manifest!=expected:raise RuntimeError('Font manifest drift')
    if manifest['sha256']!='74bd80d3e42a08517cd7e1108ba3d86f2da29ac0f3065be95e0357956ab9db37':raise RuntimeError('Pinned font SHA256 drift')
    if manifest['license_sha256']!='7c940e28a5388e9bba866cf0e408edda45fe0899ba98665b8f6ab31dc5e4b8ff':raise RuntimeError('Pinned license SHA256 drift')
    for key,digest in [('file','sha256'),('license','license_sha256')]:
        path=directory/manifest[key]
        if path.name!=manifest[key] or path.is_symlink() or not path.is_file():raise RuntimeError('Missing/invalid font resource: '+str(path))
        if hashlib.sha256(path.read_bytes()).hexdigest()!=manifest[digest]:raise RuntimeError('Font resource SHA256 mismatch: '+str(path))
    faces=manifest['faces']
    if len(faces)!=4 or {f['postscript'] for f in faces}!={'SourceCodePro-Regular','SourceCodePro-Bold','SourceCodePro-It','SourceCodePro-BoldIt'}:raise RuntimeError('Incomplete font family')
    for face in faces:
        path=directory/face['file']
        if face['file']!=face['postscript']+'.ttf' or path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest()!=face['sha256']:raise RuntimeError('Font face mismatch: '+face['postscript'])
    print('MACOS FONT ASSETS PASS:',directory)
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--bundle',type=Path);args=parser.parse_args()
    verify(SOURCE)
    if args.bundle:
        base=args.bundle/'Contents/Resources' if (args.bundle/'Contents/Resources').is_dir() else args.bundle
        verify(base/'Fonts')
