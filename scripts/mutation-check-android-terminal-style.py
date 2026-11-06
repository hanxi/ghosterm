#!/usr/bin/env python3
"""A7 terminal style and existing owner mutations on a real device, with independent Core controls."""
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
MARK = 'GT_ANDROID_TERMINAL_STYLE_MUTATION'
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
    base = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm'
    model = base / 'SessionsViewModel.kt'
    mapper = base / 'TerminalPreferences.kt'
    screen = base / 'TerminalScreen.kt'
    bridge = ROOT / 'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/NativeSession.kt'
    view = ROOT / 'terminal/android/src/main/java/com/hanxi/ghosterm/terminal/TerminalView.kt'
    native = ROOT / 'terminal/android/native/surface.c'
    dto = ROOT / 'terminal/android/src/main/java/com/hanxi/ghosterm/terminal/TerminalStyle.kt'
    theme = ROOT / 'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/AppPreferences.kt'
    originals = {path: path.read_bytes() for path in (model, mapper, screen, bridge, view, native, dto, theme)}
    modes = {path: path.stat().st_mode & 0o777 for path in originals}
    if any(MARK.encode() in data for data in originals.values()): raise RuntimeError('Residual terminal style mutation')
    api = helper.run([*adb, 'shell', 'getprop', 'ro.build.version.sdk']); helper.require(api)
    density = helper.run([*adb, 'shell', 'wm', 'density']); helper.require(density)
    if int(api.stdout.strip()) < 36 or int(re.findall(r': (\d+)', density.stdout)[-1]) <= 160:
        raise RuntimeError('Terminal style mutation profile requires API 36+ and density above 160dpi for SP geometry teeth')
    cases = []
    def case(name, test, path, old, new, count=1): cases.append((name, 'TerminalPreferencesTest#'+test, ((path,old,new,count),)))
    ack = 'acknowledgedAppearanceUpdatesTheExistingGridAndDefaultCursor'
    owners = 'committedPreferencesReachExistingAndNewOwnersWithoutReplacingShells'
    pixels = 'liveCanvasStyleResizesTheSamePtyAndPaintsTheNewPalette'
    blinking = 'disablingCursorBlinkKeepsProgramBlinkStaticWithoutDisablingAnsiBlinkText'
    packets = 'malformedNativeAppearancePacketsAreRejectedBeforeChangingTheGrid'
    case('native-background', ack, native, 'GhosttyColorRgb background = {bytes[0], bytes[1], bytes[2]};', 'GhosttyColorRgb background = {23, 27, 32};')
    case('native-foreground', ack, native, 'GhosttyColorRgb foreground = {bytes[3], bytes[4], bytes[5]};', 'GhosttyColorRgb foreground = {220, 227, 236};')
    case('native-cursor-shape', ack, native, 'GhosttyTerminalCursorStyle cursor = (GhosttyTerminalCursorStyle)x;', 'GhosttyTerminalCursorStyle cursor = GHOSTTY_TERMINAL_CURSOR_STYLE_BLOCK;')
    case('native-default-blink', ack, native, 'bool blink = y != 0;', 'bool blink = true;')
    case('preserve-program-cursor', 'programCursorOverridesSurviveDefaultsUntilDecscusrReset', native, '/* Default setters preserve',
        'ghostty_terminal_vt_write(s->terminal, (const uint8_t *)"\\033[0 q", 5); /* Default setters preserve')
    case('preserve-osc-override', 'oscOverridesSurviveNewDefaultsAndResetToTheNewPalette', native, '/* Default setters preserve',
        'ghostty_terminal_vt_write(s->terminal, (const uint8_t *)"\\033]111\\007", 6); /* Default setters preserve')
    case('no-settings-in-pty-stream', 'appearanceChangesNeverEnterTheShellInputStream', native,
        'if (length != 57 || x < 0', '(void)s->write(s->context, (const uint8_t *)"INJECTED_", 9); if (length != 57 || x < 0')
    case('bridge-color-order', ack, bridge, 'listOf(appearance.background,appearance.foreground,appearance.cursorColor)', 'listOf(appearance.foreground,appearance.background,appearance.cursorColor)')
    case('bridge-acknowledged-route', ack, bridge, 'action(12, appearance.cursor.code, if (appearance.blink) 1 else 0, colors)', 'Unit')
    case('registration-independent-of-ui', owners, model, 'surfaceTargets.value = sessions.toList()', 'surfaceTargets.value = emptyList()', 2)
    case('all-existing-owners', owners, model, 'for (session in targets)', 'for (session in targets.takeLast(1))')
    case('hot-existing-appearance', owners, model, 'if (session.appliedAppearance == style.appearance) continue', 'if (session.appliedAppearance != null) continue')
    case('paper-palette-map', owners, theme, 'TerminalTheme("paper","纸白",0xfaf8f2,0x283b33', 'TerminalTheme("paper","纸白",0x171b20,0xdce3ec')
    case('existing-view-update', pixels, screen, 'view.updateStyle(model.terminalStyle)', 'Unit')
    case('physical-line-height', pixels, view, 'ceil(paint.textSize * value.lineHeight)', 'ceil(paint.textSize)')
    case('cursor-blink-off-gate', blinking, view, '!style.appearance.blink || !frame.cursorBlink', '!frame.cursorBlink')
    case('ansi-blink-independent', blinking, view, '(cell.flags and 128 != 0 && !blink)', '(cell.flags and 128 != 0 && !blink && style.appearance.blink)')
    case('window-focus-not-only-view-focus', 'anUnfocusedWindowShowsAStaticCursorEvenWhenTheViewKeepsFocus', view,
        'val cursorFocused = hasFocus() && hasWindowFocus()', 'val cursorFocused = hasFocus()')
    case('initial-equal-style-still-measures', pixels, view, 'if (style == value && !changedMetrics) return', 'if (style == value) return')
    case('line-height-mapping', pixels, mapper, 'TerminalStyle(fontSize.toFloat(),lineHeight.factor,', 'TerminalStyle(fontSize.toFloat(),1.85f,')
    case('surface-errors-visible', 'oneUnavailableSurfaceReportsAnErrorWithoutBlockingOtherOwners', model,
        'if (session in sessions) reportError(', 'if (false) reportError(')
    case('forest-palette-map', owners, theme, 'TerminalTheme("forest","墨绿",0x14241c,0xe2ecdf', 'TerminalTheme("forest","墨绿",0x171b20,0xdce3ec')
    case('sp-physical-conversion', pixels, view,
        'TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, value.fontSize, resources.displayMetrics)', 'value.fontSize')
    case('native-blink-flag-bound', packets, native, 'y > 1) {\n            return 2;\n        }', 'y > 2) {\n            return 2;\n        }')
    case('native-exact-packet-length', packets, native, 'if (length != 57 || x < 0', 'if (length < 57 || x < 0')
    case('validate-before-palette-side-effects', packets, native, 'if (kind == 12) {',
        'if (kind == 12) { GhosttyColorRgb premature_background = {0,0,0}; (void)ghostty_terminal_set(s->terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &premature_background);')
    case('bar-cursor-abi', ack, dto, 'BAR(0)', 'BAR(1)')
    case('block-cursor-abi', ack, dto, 'BLOCK(1)', 'BLOCK(0)')
    case('underline-cursor-abi', ack, dto, 'UNDERLINE(2)', 'UNDERLINE(1)')
    selected = args.selected or list(range(1, len(cases) + 1))
    if len(selected) != len(set(selected)) or any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        for path, old, new, count in cases[number - 1][2]:
            if originals[path].decode().count(old) != count:
                raise RuntimeError(f'V{number}: source anchor missing or ambiguous')
    work = ROOT / 'tmp/android-terminal-style-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    for path, data in originals.items():
        (work / path.name).write_bytes(data)
    print('TERMINAL STYLE RECEIPTS', work, flush=True)

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
            label = f'V{number:02d}-{name}'
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
    print(f'ANDROID TERMINAL STYLE MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
