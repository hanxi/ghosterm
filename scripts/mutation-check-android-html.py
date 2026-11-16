#!/usr/bin/env python3
"""Android fixed HTML preview: native rendering, immutable attachments and view lifetime on an explicitly selected device.

Requires one normal assertion red, same-APK independent Core green, restored
scenario green, and unchanged SHA256. Does not open any host UI window.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_HTML_PREVIEW_MUTATION'
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial: raise RuntimeError('Set ANDROID_SERIAL to the explicitly intended test device')
    adb = ['adb', '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    base = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm'
    document = base / 'PluginResourceHTMLDocument.kt'
    view = base / 'PluginResourceHTMLView.kt'
    settings = base / 'SettingsContent.kt'
    originals = {p: p.read_bytes() for p in [document, view, settings]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    model = 'com.hanxi.ghosterm.PluginResourceHTMLTest#'
    native = 'com.hanxi.ghosterm.PluginResourceHTMLViewTest#'
    strict = model + 'htmlRequiresConfirmedExactBytesStrictUtf8AndCanonicalNames'
    bundles = model + 'onlyFollowingExplicitBundleAssetsAreCapturedAndOriginalArraysCannotChangeThem'
    duplicates = model + 'duplicatePathsBadMetadataAndExecutableImageTypesStayUnavailable'
    limits = model + 'attachmentCountCssBytesAndTotalDecodedPixelsHaveSeparateLimits'
    requests = model + 'syntheticOriginServesOnlyMappedGetSubresourcesAndCloseRevokesIt'
    render = native + 'nativeWebViewRendersOnlyFixedCssAndPngWithOfflineConfiguration'
    lifetime = native + 'selectingImageSourceAndRemovingPanelRevokeRetainedClientsAndUseNewOrigins'
    cases = [
        ('document-hash', strict, document, 'return bytes.takeIf { sha(it) == d.sha256 }', 'return bytes'),
        ('document-known-size', strict, document, '!d.sizeKnown || ', ''),
        ('document-parent-path', strict, document, ' && it != ".."', ''),
        ('document-byte-limit', strict, document, 'MAX_DOCUMENT_BYTES = 262144', 'MAX_DOCUMENT_BYTES = 262145'),
        ('following-bundle', bundles, document, 'resources.drop(positions.single() + 1)', 'resources.drop(0)'),
        ('next-document-fence', bundles, document,
         '!supports(it.descriptor.mime) && !PluginResourceMarkdownDecoder.supports(it.descriptor.mime, it.descriptor.displayName)', 'true'),
        ('duplicate-path', duplicates, document, 'assets.remove(path); omitted = true; continue', 'omitted = true; continue'),
        ('css-byte-limit', limits, document, 'bytes.size <= MAX_DOCUMENT_BYTES && utf8(bytes) != null', 'utf8(bytes) != null'),
        ('decoded-pixel-total', limits, document, 'pixels > remainingPixels || ', ''),
        ('attachment-count', limits, document, 'MAX_ATTACHMENTS = 6', 'MAX_ATTACHMENTS = 7'),
        ('form-policy', model + 'documentHasARestrictiveFirstPolicyAndLiteralUnicodeSource', document,
         "form-action 'none'", "form-action 'self'"),
        ('request-origin', requests, view, 'uri.rawAuthority == host && ', ''),
        ('request-method', requests, view, 'method == "GET" && ', ''),
        ('main-document-scope', requests, view, 'if (url == documentURL)', 'if (true)'),
        ('closed-view-revocation', lifetime, view, 'content = null; stylesheet = byteArrayOf()', 'stylesheet = byteArrayOf()'),
        ('network-block', render, view, 'blockNetworkLoads = true', 'blockNetworkLoads = false'),
        ('javascript-disabled', render, view, 'javaScriptEnabled = false', 'javaScriptEnabled = true'),
        ('file-access-disabled', render, view, 'allowFileAccess = false', 'allowFileAccess = true'),
        ('unique-origin', lifetime, view, 'UUID.randomUUID().toString().replace("-", "")', '"fixed"'),
        ('native-html-entry', render, settings, 'if (PluginResourceHTMLDocument.supports(mime))',
         'if (false && PluginResourceHTMLDocument.supports(mime))'),
        ('navigation', native + 'documentLinksNeverNavigateOrLaunchAnotherActivity', view,
         'override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest) = true\n        @Suppress',
         'override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest) = false\n        @Suppress'),
        ('exact-path-codepoints', model + 'decomposedDirectoryAndDistinctAccentNamesKeepTheirExactUtf8Paths', view,
         'Uri.Builder().scheme("https").authority(host).path("/${snapshot.path}").build().toString()',
         'URI("https", null, host, -1, "/${snapshot.path}", null, null).toASCIIString()'),
        ('request-utf8', model + 'malformedUtf8RequestsCannotAliasValidReplacementCharacterNames', view,
         'PluginResourceHTMLDocument.requestPath(uri!!.rawPath)',
         'uri!!.path?.removePrefix("/")?.takeIf(PluginResourceHTMLDocument::canonical)'),
        ('single-percent-decode', model + 'malformedUtf8RequestsCannotAliasValidReplacementCharacterNames', document,
         'utf8(output.copyOf(count))?.removePrefix("/")',
         'utf8(output.copyOf(count))?.let { java.net.URLDecoder.decode(it, "UTF-8") }?.removePrefix("/")'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-html-preview-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    print('RECEIPTS ' + str(work), flush=True)
    for path, data in originals.items(): (work / path.name).write_bytes(data)
    def write(path, data):
        temporary = work / ('source-' + str(time.time_ns()))
        temporary.write_bytes(data); temporary.chmod(modes[path]); os.replace(temporary, path)
    def restore():
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes(): write(path, data)
            if path.read_bytes() != data: raise RuntimeError('Restore SHA mismatch: ' + str(path))
    def logged(label, command, seconds=120):
        result = helper.run(command, seconds)
        output = result.stdout + result.stderr
        (work / (label + '.log')).write_text(output)
        helper.require(result); return output
    def build(label):
        # Compile the production renderer, real WebView test and independent Core control.
        logged(label + '-build', ['bash', ROOT / 'scripts/build-android.sh', '-PghostermPluginJs=true',
            ':app:assembleDebug', ':app:assembleDebugAndroidTest'], 900)
        for relative in ['debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk']:
            logged(label + '-install-' + Path(relative).name, [*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs/apk' / relative])
    def test(label, method, green):
        output = logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', method,
            'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        if 'INSTRUMENTATION_STATUS_CODE: -4' in output or 'assumption failed:' in output:
            raise RuntimeError('Skipped instead of executed:\n' + output)
        good = ('OK (1 test)' in output and 'FAILURES' not in output) if green else (
            bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and
            any(t in output for t in ['java.lang.AssertionError', 'org.junit.ComparisonFailure']) and
            'Process crashed' not in output and 'INSTRUMENTATION_FAILED' not in output)
        if not green:
            # One selected JUnit method must fail by a normal assertion, never
            # compilation, process death, sanitizer, setup or runner failure.
            good = good and method.split('#')[1] in output
        if not good or 'INSTRUMENTATION_STATUS: numtests=1' not in output: raise RuntimeError('Not accepted assertion result:\n' + output)
    control = 'com.hanxi.ghosterm.NativeSmokeTest#corePreservesUtf8AndLineEndings'
    results = []
    try:
        build('baseline')
        for index, target in enumerate(dict.fromkeys(c[1] for c in cases)): test('baseline-' + str(index), target, True)
        for name, target, path, before, after in cases:
            write(path, (originals[path].decode().replace(before, after) + '\n// ' + MARK + '\n').encode())
            build(name); test(name + '-target', target, False); test(name + '-control', control, True)
            restore(); build(name + '-restored'); test(name + '-restored', target, True)
            results.append(dict(case=name, compiled_assertion_red=True, same_apk_core_green=True, restored_green=True))
            (work / 'accepted-results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(name + ': compiled normal assertion RED / Core GREEN / restored GREEN / SHA256', flush=True)
    finally:
        restore()
    for path, data in originals.items(): print(str(path.relative_to(ROOT)) + ' SHA256 ' + hashlib.sha256(data).hexdigest(), flush=True)
    print('ANDROID HTML PREVIEW MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
