#!/usr/bin/env python3
"""Nerd font renderer device mutations on a real device, with independent Core controls."""
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
MARK = 'GT_ANDROID_FONT_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', type=int, action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise RuntimeError('Set ANDROID_SERIAL to the preference test device')
    adb = ['adb', '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    manifest = ROOT / 'terminal/android/src/main/java/com/hanxi/ghosterm/terminal/TerminalView.kt'
    originals = {manifest: manifest.read_bytes()}
    modes = {path: path.stat().st_mode & 0o777 for path in originals}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual settings mutation')
    cases = [('renderer-font', 'NerdFontTest#installedRendererCoversNerdSymbolsAndRetainsSystemTextFallback', (
        (manifest, 'paint.typeface=face;metricsPaint.typeface=face', 'paint.typeface=Typeface.MONOSPACE;metricsPaint.typeface=face', 1),
    )), ('symbol-fit', 'NerdFontTest#privateUseInkFitsWithoutRelyingOnCellClipping', (
        (manifest, 'if (!fitsSymbolCell(cell.text))', 'if (true)', 1),
    )), ('normal-symbol-fit', 'NerdFontTest#normalAndBlockCursorUseTheCompleteFittedGlyph', (
        (manifest, 'drawCellText(canvas, cell, left, top)', 'canvas.drawText(cell.text, left, top + baseline, paint)', 1),
    )), ('cursor-symbol-fit', 'NerdFontTest#normalAndBlockCursorUseTheCompleteFittedGlyph', (
        (manifest, 'drawCellText(canvas,cell,x,y)', 'canvas.drawText(cell.text,x,y+baseline,paint)', 1),
    )), ('symbol-metrics-cache', 'NerdFontTest#privateUseInkFitsWithoutRelyingOnCellClipping', (
        (manifest, 'symbolPlacements.evictAll()', '/* stale symbol metrics */', 1),
    )), ('symbol-raster-edge', 'NerdFontTest#privateUseInkFitsWithoutRelyingOnCellClipping', (
        (manifest, 'val inset = minOf(1f, width / 4, height / 4)', 'val inset = 0f', 1),
    )), ('unicode-symbol-fit', 'NerdFontTest#promptSymbolsFitWithoutClipping', (
        (manifest, 'if (!fitsSymbolCell(cell.text))', 'if (Character.getType(cell.text.codePointAt(0)) != Character.PRIVATE_USE.toInt())', 1),
    )), ('bitmap-emoji-bounds', 'NerdFontTest#colorEmojiFitWithoutClipping', (
        (manifest, 'paint.getTextBounds(cell.text, 0, cell.text.length, symbolRasterBounds)', 'symbolRasterBounds.setEmpty()', 1),
    )), ('joining-edges', 'NerdFontTest#textBaselineAndJoiningEdgesRemainUnchanged', (
        (manifest, 'if (cp in 0x2500..0x259f) return false', 'if (cp in 0x2500..0x259f) return true', 1),
    )), ('bitmap-italic-bounds', 'NerdFontTest#colorEmojiFitWithoutClipping', (
        (manifest, 'if (symbolOutline.isEmpty && !symbolRasterBounds.isEmpty)', 'if (false)', 1),
    )), ('rounded-powerline-edges', 'NerdFontTest#roundedPowerlineCapsJoinStatusBackground', (
        (manifest, 'if (cell.text == "\\ue0b4" || cell.text == "\\ue0b6")', 'if (false)', 1),
    ))]
    sdk = int(helper.run([*adb, 'shell', 'getprop', 'ro.build.version.sdk']).stdout.strip())
    selected = args.selected or (list(range(1, 10)) + ([10] if sdk == 29 else []) + [11])
    if 10 in selected and sdk != 29:
        raise RuntimeError('Bitmap italic metrics regression requires the API 29 test device')
    if len(selected) != len(set(selected)) or any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        for path, old, new, count in cases[number - 1][2]:
            if originals[path].decode().count(old) != count:
                raise RuntimeError(f'C{number}: source anchor missing or ambiguous')
    work = ROOT / 'tmp/android-font-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    for path, data in originals.items():
        (work / path.name).write_bytes(data)
    print('SETTINGS RECEIPTS', work, flush=True)

    def write_source(path, data):
        temporary = work / ('source-' + str(time.time_ns()) + '.kt')
        temporary.write_bytes(data)
        temporary.chmod(modes[path])
        os.replace(temporary, path)

    def restore():
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes():
                write_source(path, data)
            if path.read_bytes() != data:
                raise RuntimeError('Settings source restore SHA256 mismatch: ' + path.name)

    def run_logged(label, command, seconds=120):
        result = helper.run(command, seconds)
        output = result.stdout + result.stderr
        (work / (label + '.log')).write_text(output)
        helper.require(result)
        return output

    def build(label):
        run_logged(label + '-build', ['bash', ROOT / 'scripts/build-android.sh'], 900)
        for name in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
            run_logged(label + '-install-' + Path(name).name, [*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / name])

    def test(label, target, green):
        output = run_logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
            'com.hanxi.ghosterm.' + target, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        if green:
            good = 'OK (1 test)' in output and 'FAILURES' not in output
        else:
            good = bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and any(
                kind in output for kind in ('java.lang.AssertionError', 'org.junit.ComparisonFailure'))
            good = good and 'Fixture:' not in output
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output or not good:
            raise RuntimeError(output)

    results = []
    try:
        build('baseline')
        for index, target in enumerate(dict.fromkeys(cases[number - 1][1] for number in selected)):
            test('baseline-' + str(index), target, True)
        for number in selected:
            name, target, edits = cases[number - 1]
            variants = {}
            for path, old, new, count in edits:
                variants[path] = variants.get(path, originals[path].decode()).replace(old, new)
            for path, source in variants.items():
                write_source(path, (source + '\n// ' + MARK + '\n').encode())
            label = f'C{number:02d}-{name}'
            build(label)
            test(label + '-target', target, False)
            test(label + '-control', 'ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', True)
            restore()
            build(label + '-restored')
            test(label + '-restored', target, True)
            results.append(dict(case=number, name=name, compiled_assertion_red=True, same_apk_core_green=True, restored_green=True))
            (work / 'accepted-results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(label + ': compiled single assertion red / same APK Core green / restored green / SHA256', flush=True)
    finally:
        restore()
        helper.require(helper.run([*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm']))
    print(f'ANDROID FONT MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
