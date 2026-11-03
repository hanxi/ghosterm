#!/usr/bin/env python3
"""Compile design mutations and prove real resource/component assertions catch them."""
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
MARK = 'GT_ANDROID_DESIGN_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append', dest='selected')
    args = parser.parse_args()
    adb = ['adb', '-s', os.environ['ANDROID_SERIAL']]
    base = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm'
    theme, components = base / 'GhostermTheme.kt', base / 'GhostermComponents.kt'
    originals = {p: p.read_bytes() for p in (theme, components)}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual design mutation')
    palette = 'AndroidDesignResourcesTest#nativeSemanticColorsMatchBothReviewedHtmlPalettes'
    geometry = 'AndroidDesignComponentsTest#sharedIconPreservesRequestedGeometryAndThemeTint'
    cases = [
        ('light-palette', theme, 'else Color(0xFFF7F8FC)', 'else Color(0xFFFFFFFF)', palette),
        ('dark-palette', theme, 'val bg=if(dark) Color(0xFF171B20)', 'val bg=if(dark) Color(0xFF000000)', palette),
        ('requested-icon', components, 'painterResource(icon.resource)', 'painterResource(UiIcon.Folder.resource)', geometry),
        ('theme-tint', components, 'modifier=modifier.size(GhostermDimensions.icon),tint=tint)',
            'modifier=modifier.size(GhostermDimensions.icon),tint=Color.Red)', geometry),
        ('accessible-label', components, 'GhostermIcon(icon,label,tint=', 'GhostermIcon(icon,null,tint=',
            'AndroidDesignComponentsTest#sharedIconButtonHasAccessibleLabelAndMinimumTouchTarget'),
    ]
    selected = set(args.selected or [c[0] for c in cases])
    if selected - {c[0] for c in cases}:
        raise RuntimeError('Unknown design case')
    cases = [c for c in cases if c[0] in selected]
    for name, path, old, new, target in cases:
        if originals[path].decode().count(old) != 1:
            raise RuntimeError(f'{name}: missing or ambiguous source anchor')
    work = ROOT / 'tmp/android-design-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    for path, data in originals.items():
        (work / path.name).write_bytes(data)
    print('DESIGN RECEIPTS', work, flush=True)

    def logged(label, command, seconds=180):
        result = helper.run(command, seconds)
        output = result.stdout + result.stderr
        (work / (label + '.log')).write_text(output)
        helper.require(result)
        return output

    def build(label):
        logged(label + '-build', ['bash', ROOT / 'scripts/build-android.sh'], 900)
        for apk in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
            logged(label + '-install-' + Path(apk).name, [*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / apk])

    def test(label, target, green):
        output = logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
            'com.hanxi.ghosterm.' + target, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        good = ('OK (1 test)' in output and 'FAILURES' not in output) if green else (
            bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and
            any(t in output for t in ('java.lang.AssertionError', 'org.junit.ComparisonFailure')))
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output or not good:
            raise RuntimeError(output)

    def restore():
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes():
                path.write_bytes(data)
            if path.read_bytes() != data:
                raise RuntimeError('Design restore SHA256 mismatch')

    accepted = []
    try:
        build('baseline')
        for index, target in enumerate(dict.fromkeys(c[-1] for c in cases)):
            test(f'baseline-{index}', target, True)
        for name, path, old, new, target in cases:
            path.write_text(originals[path].decode().replace(old, new) + '\n// ' + MARK + '\n')
            build(name)
            test(name + '-target', target, False)
            test(name + '-control', 'ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', True)
            restore()
            build(name + '-restored')
            test(name + '-restored', target, True)
            accepted.append(dict(name=name, compiled_assertion_red=True, same_apk_core_green=True,
                restored_green=True, sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in originals}))
            (work / 'accepted-results.json').write_text(json.dumps(accepted, indent=2) + '\n')
            print('PASS ' + name + ': compiled assertion red / same APK Core green / restored green / SHA256', flush=True)
    finally:
        restore()
    print(f'ANDROID DESIGN MUTATIONS PASS: {len(accepted)} groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
