#!/usr/bin/env python3
"""Theme and custom font device mutations on a real device, with independent Core controls."""
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
MARK = 'GT_ANDROID_APPEARANCE_MUTATION'
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
    terminal=ROOT/'terminal/android/src/main/java/com/hanxi/ghosterm/terminal'
    view=terminal/'TerminalView.kt'
    store=terminal/'TerminalFontStore.kt'
    fonts=terminal/'TerminalFonts.kt'
    native=ROOT/'terminal/android/native/surface.c'
    mapper=ROOT/'android/app/src/main/java/com/hanxi/ghosterm/TerminalPreferences.kt'
    originals={p:p.read_bytes() for p in (view,store,fonts,native,mapper)}
    modes={p:p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in data for data in originals.values()): raise RuntimeError('Residual appearance mutation')
    cases=[]
    def case(name,test,path,old,new): cases.append((name,test,((path,old,new,1),)))
    palette='TerminalPreferencesTest#allAnsiColorsAndOscCursorOverridesUseTheSelectedTheme'
    canvas='TerminalPreferencesTest#selectedThemeSelectionAndCursorReachActualCanvasPixels'
    case('ansi-palette',palette,native,'for (size_t i=0;i<16;i++) palette[i]=','for (size_t i=1;i<16;i++) palette[i]=')
    case('cursor-default',palette,native,'GhosttyColorRgb cursor_color = {bytes[6], bytes[7], bytes[8]};','GhosttyColorRgb cursor_color = {0,0,0};')
    case('selection-background',canvas,view,'or style.appearance.selectionBackground','or 0x445b7d')
    case('selection-text',canvas,view,'or style.appearance.selectionForeground','or 0xffffff')
    case('font-mapping','TerminalAppearanceProductTest#selectedFontsChangeTheExistingViewMetricsAndRenderingWithoutReplacingThePty',mapper,'theme.palette),fontFamily.id)','theme.palette),"system")')
    case('font-validation','TerminalAppearanceCatalogTest#proportionalSymbolTruncatedCollectionAndOversizedFilesAreRejectedWithoutPublishing',store,'MonospaceFont.validate(bytes)','Unit')
    case('warm-cache-integrity','TerminalAppearanceCatalogTest#warmTypefaceCacheCannotHideChangedOrMissingPrivateFont',store,'check(hash(path.readBytes())==id.removePrefix("custom:"))','check(true)')
    case('storage-budget','TerminalAppearanceCatalogTest#quotaIncludesResidualFilesAndDoesNotLeaveAnotherStage',store,'existing.sumOf { it.length() }+bytes.size+metadataBytes.size<=MAX_TOTAL_BYTES','bytes.size+metadataBytes.size<=MAX_TOTAL_BYTES')
    case('metadata-budget','TerminalAppearanceCatalogTest#quotaIncludesResidualFilesAndDoesNotLeaveAnotherStage',store,'existing.sumOf { it.length() }+bytes.size+metadataBytes.size<=MAX_TOTAL_BYTES','existing.sumOf { it.length() }+bytes.size<=MAX_TOTAL_BYTES')
    case('cursor-text',canvas,view,'or style.appearance.cursorText','or 0xffffff')
    selected = args.selected or list(range(1, len(cases) + 1))
    if len(selected) != len(set(selected)) or any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        for path, old, new, count in cases[number - 1][2]:
            if originals[path].decode().count(old) != count:
                raise RuntimeError(f'C{number}: source anchor missing or ambiguous')
    work = ROOT / 'tmp/android-appearance-mutations' / str(time.time_ns())
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
    print(f'ANDROID APPEARANCE MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
