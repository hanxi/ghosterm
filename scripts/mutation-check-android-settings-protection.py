#!/usr/bin/env python3
"""Reset protection against actual files, Keystore, a shell and the process file owner."""
import hashlib
import json
import os
import re
import signal
import time

MARK = 'GT_ANDROID_SETTINGS_PROTECTION_MUTATION'
TARGET = 'com.hanxi.ghosterm.SettingsScreenTest#resetPreservesLiveShellProtectedFilesIdentityClipboardAndQueuedRequest'
CONTROL = 'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'


def execute(root, build, test):
    source = root / 'android/app/src/main/java/com/hanxi/ghosterm/SettingsContent.kt'
    original = source.read_bytes()
    mode = source.stat().st_mode & 0o777
    if MARK.encode() in original:
        raise RuntimeError('Residual settings protection mutation')
    work = root / 'tmp/android-settings-protection-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    (work / source.name).write_bytes(original)
    print('SETTINGS PROTECTION RECEIPTS', work, flush=True)
    context_anchor = '    val p = model.candidate'
    button_anchor = 'TextButton(model::confirmReset,'
    text = original.decode()
    if text.count(context_anchor) != 1 or text.count(button_anchor) != 1:
        raise RuntimeError('Settings protection source anchor missing or ambiguous')
    cases = [
        ('config', 'java.io.File(app.filesDir, "home/.ssh/config").delete()'),
        ('trust', 'java.io.File(app.filesDir, "home/.ssh/known_hosts").delete()'),
        ('files', 'java.io.File(app.filesDir, "home").listFiles()?.filter { it.name.startsWith("reset-protection-") }?.forEach { it.deleteRecursively() }'),
        ('identity', 'java.io.File(app.filesDir, "identities").listFiles()?.filter { it.extension == "json" && runCatching { org.json.JSONObject(it.readText()).optString("name").startsWith("reset-protection-") }.getOrDefault(false) }?.forEach { it.delete() }'),
        ('clipboard', 'app.fileWork.clipboard.snapshot = null'),
        ('queued-request', 'app.fileWork.tasks.tasks.filter { it.request.sourcePathLabel.startsWith("reset-protection-") }.toList().forEach(app.fileWork.tasks::cancel)'),
        ('live-shell', 'protectionActivity?.let { activity -> val sessions = androidx.lifecycle.ViewModelProvider(activity as androidx.lifecycle.ViewModelStoreOwner)[SessionsViewModel::class.java]; sessions.active?.let { sessions.close(it.id) } }'),
    ]
    requested = os.environ.get('ANDROID_SETTINGS_PROTECTION_CASES', '')
    if requested:
        selected = requested.split(',')
        if len(selected) != len(set(selected)) or set(selected) - {name for name, _ in cases}:
            raise RuntimeError('Invalid or duplicate settings protection mutation')
        cases = [(name, body) for name, body in cases if name in selected]

    def write(value):
        temporary = work / ('source-' + str(time.time_ns()) + '.kt')
        temporary.write_bytes(value)
        temporary.chmod(mode)
        os.replace(temporary, source)

    def restore():
        current = source.read_bytes()
        if MARK.encode() in current:
            write(original)
        if hashlib.sha256(source.read_bytes()).digest() != hashlib.sha256(original).digest():
            raise RuntimeError('Settings protection source SHA256 restore mismatch')

    def assertion(label, selector, green):
        output = test(label, selector, green)
        (work / (label + '.log')).write_text(output)
        expected_class, expected_test = selector.split('#')
        events, state = [], {}
        for line in output.splitlines():
            if line.startswith('INSTRUMENTATION_STATUS: '):
                key, separator, value = line.removeprefix('INSTRUMENTATION_STATUS: ').partition('=')
                if separator:
                    state[key] = value
            elif line.startswith('INSTRUMENTATION_STATUS_CODE: '):
                events.append((int(line.split(': ', 1)[1]), state))
                state = {}
        starts = [s for code, s in events if code == 1]
        ends = [s for code, s in events if code == (0 if green else -2)]
        valid = len(events) == 2 and len(starts) == len(ends) == 1 and all(
            s.get('class') == expected_class and s.get('test') == expected_test and s.get('numtests') == '1'
            for _, s in events)
        if green:
            valid = valid and 'OK (1 test)' in output and 'FAILURES' not in output
        else:
            valid = valid and bool(re.search(r'Tests run: 1,\s+Failures: 1', output))
            valid = valid and 'java.lang.AssertionError' in output and 'Fixture:' not in output
        if not valid:
            raise RuntimeError('Reset protection requires one exact compiled assertion: ' + label)
        return output

    def interrupted(_signum, _frame):
        raise KeyboardInterrupt()

    handlers = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    results = []
    try:
        for index, (name, body) in enumerate(cases, 1):
            patched = text.replace(context_anchor,
                '    val protectionContext = androidx.compose.ui.platform.LocalContext.current\n'
                '    val protectionActivity = androidx.activity.compose.LocalActivity.current\n' + context_anchor)
            patched = patched.replace(button_anchor,
                'TextButton({ model.confirmReset(); val app = protectionContext.applicationContext as GhostermApplication; ' + body + ' },')
            write((patched + '\n// ' + MARK + '\n').encode())
            label = f'R{index:02d}-{name}'
            build(label)
            output = assertion(label + '-target', TARGET, False)
            assertion(label + '-control', CONTROL, True)
            restore()
            build(label + '-restored')
            assertion(label + '-restored', TARGET, True)
            results.append(dict(case=name, compiled_single_assertion_red=True, same_apk_core_green=True,
                restored_green=True, source_sha256=hashlib.sha256(original).hexdigest(),
                assertion=re.search(r'java.lang.AssertionError[^\n]*', output).group(0)))
            (work / 'accepted-results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(label + ': compiled single assertion red / same APK Core green / restored green / SHA256', flush=True)
    finally:
        restore()
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
    print(f'ANDROID SETTINGS PROTECTION MUTATIONS PASS: {len(results)} groups', flush=True)


if __name__ == '__main__':
    raise SystemExit('Use ANDROID_SETTINGS_PROTECTION_MUTATE=1 with test-android-ssh.sh and SettingsScreenTest#resetPreservesLiveShellProtectedFilesIdentityClipboardAndQueuedRequest')
