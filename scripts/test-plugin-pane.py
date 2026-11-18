#!/usr/bin/env python3
"""Exercise macOS plugin controls over owned SSH, optionally in mutated copies.

The directory chooser is injected; this does not verify the system file panel.
All windows remain offscreen and all temporary copies are under project tmp/.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PANE = 'apple/GhostermApp/Sources/GhostermApp/ExtensionsPane.swift'
RUN = 'apple/GhostermApp/Sources/GhostermApp/ExtensionRunView.swift'
HTML = 'apple/GhostermApp/Sources/GhostermApp/PluginResourceHTMLAssets.swift'
SNAPSHOT = 'apple/GhostermApp/Sources/GhostermApp/PluginResourceHTMLSnapshot.swift'
TEST = 'PluginPaneFlowTests.testPreviewThroughPackageApprovalLaunchConsentAndStopControls'
CONTROL = 'HostListTests.testReadingDoesNotMutateConfig'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gterm', type=Path, required=True)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--route', choices=('direct', 'jump', 'both'), default='both')
    parser.add_argument('--mutation-case', action='append')
    parser.add_argument('--out', type=Path, default=ROOT / 'tmp/plugin-pane-receipt.json')
    args = parser.parse_args()
    gterm = args.gterm.resolve(strict=True)
    out = args.out.resolve()
    assert ROOT / 'tmp' in out.parents, 'receipt must be under project tmp/'
    assert gterm.is_file() and os.access(gterm, os.X_OK), 'explicit executable gterm required'
    selected_markdown = TEST if args.route != 'jump' else 'PluginPaneFlowTests.testMarkdownAttachmentsThroughProxyJumpControls'
    selected_html = 'PluginPaneFlowTests.testHTMLAttachmentsThrough' + ('DirectControls' if args.route == 'direct' else 'ProxyJumpControls')
    selected_image = 'PluginPaneFlowTests.testImageThrough' + ('ProxyJumpControls' if args.route == 'jump' else 'DirectControls')
    baseline = ('PluginPaneFlowTests.test(PreviewThroughPackageApprovalLaunchConsentAndStopControls|'
                'MarkdownAttachmentsThroughProxyJumpControls|(AnimatedHTMLAttachments|UnicodeHTMLAttachments|HTMLAttachments|Image)Through(Direct|ProxyJump)Controls)$')
    if args.route == 'direct':
        baseline = 'PluginPaneFlowTests.test(PreviewThroughPackageApprovalLaunchConsentAndStopControls|(AnimatedHTMLAttachments|UnicodeHTMLAttachments|HTMLAttachments|Image)ThroughDirectControls)$'
    elif args.route == 'jump':
        baseline = 'PluginPaneFlowTests.test.*ThroughProxyJumpControls$'
    baseline_count = 10 if args.route == 'both' else 5
    cases = [
        ('picker-cancel', 'if (error as? CocoaError)?.code == .userCancelled { return }',
         '// fixture mutation: handle cancellation as an import failure',
         'cancelling the picker must not display an import error'),
        ('digest', 'Text("摘要：\\(entry.review.digest)")', 'Text("摘要：unverified")',
         'review must display the exact immutable package digest'),
        ('approval', 'decide(entry: entry, approve: true)', 'decide(entry: entry, approve: false)',
         'approval did not reveal launch form'),
        ('launch', 'Button("连接并启动 Client 命令") { launch(entry) }', 'Button("连接并启动 Client 命令") { }',
         'launch control did not reach consent'),
        ('consent', 'await model.decideExtensionConsent(allow: true)', 'await model.decideExtensionConsent(allow: false)',
         'Preview did not receive exact owned SSH resource', selected_markdown, RUN),
        ('stop', 'await model.stopExtensionRun()', 'await Task.yield()',
         'stop control did not release run', selected_markdown, RUN),
    ]
    cases.extend([
        ('markdown-image', 'PluginResourceImagePreview(data: asset.data, mime: asset.mime)', 'Text("disabled attachment")',
         'Markdown attachment did not draw actual red pixels', selected_markdown),
        ('standalone-image', 'PluginResourceImagePreview(data: resource.bytes, mime: resource.mime)', 'Text("disabled image")',
         'standalone image did not draw actual red pixels', selected_image),
        ('html-assets', 'PluginResourceHTMLAssetResolver(document: resource, resources: resources) { item in',
         'PluginResourceHTMLAssetResolver(document: resource, resources: [resource]) { item in',
         'embedded HTML did not draw received CSS and PNG', selected_html, SNAPSHOT),
        ('selection', 'selection.select(resource.resourceID, available: identifiers)', '_ = resource.resourceID',
         'resource selection did not replace embedded HTML with the selected image', selected_html),
    ])
    html_source = (ROOT / HTML).read_text()
    start = html_source.index('enum PluginResourceHTMLWebViewConfiguration {')
    end = html_source.index('\n        configuration.defaultWebpagePreferences', start)
    factory = html_source[start:end]
    shared = factory.replace('enum PluginResourceHTMLWebViewConfiguration {',
        'enum PluginResourceHTMLWebViewConfiguration {\n    private static let sharedStore = WKWebsiteDataStore.nonPersistent()')
    shared = shared.replace('configuration.websiteDataStore = .nonPersistent()', 'configuration.websiteDataStore = Self.sharedStore')
    fresh = 'PluginResourceUITests.testHTMLWebViewUsesFreshNonpersistentStorage'
    lifetime = 'PluginResourceUITests.testHTMLCoordinatorCloseStopsLoadingAndRejectsLateUpdates'
    handler = 'PluginResourceUITests.testHTMLClosedSchemeHandlerRejectsLateUpdateAndRequest'
    close_header = 'func close(in webView: WKWebView) {\n        guard !closed else { return }'
    update_header = 'func update(snapshot: PluginResourceHTMLSnapshot, in webView: WKWebView) {\n        guard !closed else { return }'
    view_creation = 'let webView = WKWebView(frame: .zero, configuration: configuration)'
    cases.extend([
        ('privacy-persistent', 'configuration.websiteDataStore = .nonPersistent()', 'configuration.websiteDataStore = .default()',
         'HTML previews require a fresh nonpersistent data store per view', fresh, HTML),
        ('privacy-shared-store', factory, shared,
         'HTML previews require a fresh nonpersistent data store per view', fresh, HTML),
        ('privacy-page-store', view_creation, 'configuration.websiteDataStore = .default()\n        ' + view_creation,
         'embedded HTML must use nonpersistent storage', selected_html),
        ('privacy-page-js', view_creation, 'configuration.defaultWebpagePreferences.allowsContentJavaScript = true\n        ' + view_creation,
         'embedded HTML must disable JavaScript', selected_html),
        ('privacy-dismantle', 'coordinator.close(in: webView)', 'webView.stopLoading()',
         'removed HTML must detach delegates and revoke attachment reads even while retained', selected_html),
        ('privacy-coordinator-stop', 'webView.stopLoading()', '_ = webView',
         'closed HTML coordinator must stop once, detach delegates and reject late loads', lifetime, HTML),
        ('privacy-coordinator-late', update_header, update_header.replace('guard !closed else { return }', '_ = closed'),
         'closed HTML coordinator must stop once, detach delegates and reject late loads', lifetime, HTML),
        ('privacy-coordinator-repeat', close_header, close_header.replace('guard !closed else { return }', '_ = closed'),
         'closed HTML coordinator must stop once, detach delegates and reject late loads', lifetime, HTML),
        ('privacy-coordinator-delegate', 'webView.navigationDelegate = nil', '_ = webView.navigationDelegate',
         'closed HTML coordinator must stop once, detach delegates and reject late loads', lifetime, HTML),
        ('privacy-coordinator-revoke', 'schemeHandler.close()', '_ = schemeHandler',
         'coordinator close must revoke its retained attachment handler', 'PluginResourceUITests.testHTMLCoordinatorCloseRevokesRetainedSchemeHandler', HTML),
        ('privacy-handler-close', 'resolver = nil', '_ = resolver',
         'closed HTML scheme handler must reject future reads and late updates', handler, HTML),
        ('privacy-handler-late', 'guard resolver != nil else { return }', '_ = resolver',
         'closed HTML scheme handler must reject future reads and late updates', handler, HTML),
    ])
    exact = 'PluginResourceUITests.testHTMLResolverKeepsCanonicallyEquivalentFilenamesDistinct'
    scope = 'PluginResourceUITests.testHTMLResolverRejectsEquivalentButDifferentDirectoryBytes'
    canonical = 'PluginResourceUITests.testHTMLCanonicalPathsUseSlashBytesDespiteFollowingCombiningMarks'
    valid_path = 'PluginResourceUITests.testHTMLResolverPreservesCombiningMarksAdjacentToSlashBytes'
    cases.extend([
        ('path-key-bytes', 'let key = Data(resource.displayName.utf8)',
         'let key = Data(resource.displayName.precomposedStringWithCanonicalMapping.utf8)',
         'different UTF-8 filenames must keep their own snapshot bytes', exact, HTML),
        ('path-bundle-scope', '.filter { $0.displayName.utf8.starts(with: directory) }',
         '.filter { $0.displayName.hasPrefix(String(decoding: directory, as: UTF8.self)) }',
         'equivalent Unicode directories must not widen the exact document scope', scope, HTML),
        ('path-slash-bytes', 'for part in bytes.split(separator: 47, omittingEmptySubsequences: false)',
         'for part in String(decoding: bytes, as: UTF8.self).split(separator: "/", omittingEmptySubsequences: false).map({ Data($0.utf8) })',
         'combining marks must not hide absolute or empty path components', canonical, HTML),
        ('path-url-segments', 'let encoded = bytes.split(separator: 47, omittingEmptySubsequences: false)',
         'let encoded = path.split(separator: "/", omittingEmptySubsequences: false).map { Data($0.utf8) }',
         'valid combining marks after slash bytes must retain their exact path and attachment', valid_path, HTML),
        ('path-leading-slash', 'let relative = Data(bytes.dropFirst())', 'let relative = Data(path.dropFirst().utf8)',
         'valid combining marks after slash bytes must retain their exact path and attachment', valid_path, HTML),
    ])
    budget = 'PluginHTMLImageBudgetTests.'
    consumer = budget + 'testPNGConsumerRefusesGrowthBeforeTheLimitAndKeepsFailureSticky'
    cases.extend([
        ('image-width', 'width <= UInt64(maximumDimension), ', '',
         'HTML must reject source dimensions beyond the raster preview limit',
         budget + 'testHTMLRejectsOversizedRasterBeforeServingItToWebKit', SNAPSHOT),
        ('image-height', 'height <= UInt64(maximumDimension),', '',
         'HTML must reject raster height beyond the preview limit',
         budget + 'testHTMLRejectsOversizedRasterHeightBeforeServingItToWebKit', SNAPSHOT),
        ('image-total-pixels', 'remaining -= width * height', '_ = width * height',
         'HTML attachments must share one decoded pixel budget',
         budget + 'testHTMLAppliesOneDecodedPixelBudgetToTheWholeBundle', SNAPSHOT),
        ('image-static-frame', 'return .init(data: bytes, mime: "image/png")',
         'return .init(data: item.bytes, mime: mime)',
         'HTML must receive a bounded static PNG instead of the original animation',
         budget + 'testHTMLServesOnlyTheFirstStaticFrameOfAnAnimatedRaster', SNAPSHOT),
        ('image-container', 'actual.conforms(to: expected)', 'true',
         'HTML must reject an undeclared raster container',
         budget + 'testHTMLRejectsImageBytesWhoseTypeDoesNotMatchTheDeclaredMime', SNAPSHOT),
        ('image-derived-budget', 'maximumEncodedBytes = 16_842_752', 'maximumEncodedBytes = 1_048_576',
         'source and derived byte budgets must stay separate without dropping a valid JPEG',
         budget + 'testHTMLKeepsLegitimateJpegAndItsOriginalReceiptWhenDerivedPngIsLarger', SNAPSHOT),
        ('image-encode-cap', ', count <= limit - data.count', '',
         'PNG consumer must stop before growing beyond its budget and stay failed', consumer, SNAPSHOT),
        ('image-encode-sticky', '!exceeded, ', '',
         'PNG consumer must stop before growing beyond its budget and stay failed', consumer, SNAPSHOT),
        ('image-duplicate-path', 'guard seen.insert(key).inserted else', 'guard candidates[key] == nil else',
         'a rejected raster must not release its duplicate path for another attachment',
         budget + 'testHTMLDuplicatePathStaysUnavailableWhenTheFirstRasterWasRejected', HTML),
        ('image-attachment-arrival', '.task(id: identity)', '.task(id: resource.sha256)',
         'prepared attachment changes must replace and revoke the previous HTML view',
         'PluginResourceUITests.testHTMLPreparedAttachmentArrivalRebuildsAndRevokesTheOldView'),
    ])
    if args.mutation_case and (not args.mutate or set(args.mutation_case) - {c[0] for c in cases}):
        parser.error('--mutation-case requires --mutate and a known case name')
    originals = {name: (ROOT / name).read_bytes() for name in [PANE, RUN, HTML, SNAPSHOT]}
    for label, old, _new, _expected, *options in cases:
        filename = options[1] if len(options) > 1 else PANE
        assert originals[filename].count(old.encode()) == 1, label
    tracked = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=ROOT).decode().split('\0')
    names = {p for p in tracked if p.startswith(('core/', 'apple/', 'packages/plugin-sdk/', 'plugins/official/preview/'))
             and (ROOT / p).is_file()}
    names.update([PANE, 'scripts/test-plugin-pane.py', 'scripts/apple-app-gate.sh',
                  'scripts/check-abi-apple.sh', 'scripts/build-core.sh',
                  'scripts/test-plugin-html-webkit.sh', 'scripts/lib/gt-timeout.sh',
                  'apple/GhostermApp/Tests/GhostermAppTests/PluginPaneFlowTests.swift'])
    inputs = {name: sha(ROOT / name) for name in sorted(names)}
    executable_sha = sha(gterm)
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), GHOSTERM_AUTOMATION_OFFSCREEN='1',
               DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',
               GHOSTERM_CORE_TEST_PLUGIN_JS='1', GHOSTERM_TEST_GTERM=str(gterm))
    # The gate, never an inherited endpoint, creates the only authorized SSH service.
    for key in list(env):
        if key.startswith('GHOSTERM_SSH_'):
            del env[key]
    env.pop('GHOSTERM_TEST_CODEX_JUMP', None)
    env.pop('GHOSTERM_TEST_SSH_JUMP', None)
    if args.route != 'direct':
        env['GHOSTERM_TEST_SSH_JUMP'] = '1'
    commands, groups = [], []
    with tempfile.TemporaryDirectory(prefix='pane-ui-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        source = work / 'source'
        for directory in ['apple/GhostermApp', 'apple/GhostermCore', 'apple/GhostermTerminal']:
            shutil.copytree(ROOT / directory, source / directory, symlinks=True,
                            ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        (source / 'core').mkdir()
        for name in ['include', 'build']:
            (source / 'core' / name).symlink_to(ROOT / 'core' / name, target_is_directory=True)
        for name in ['build', 'patches', 'plugins']:
            (source / name).symlink_to(ROOT / name, target_is_directory=True)
        (source / 'tmp').mkdir()
        env['GHOSTERM_APP_TEST_PACKAGE'] = str(source / 'apple/GhostermApp')

        def run(label, selected=selected_markdown, expected_failure=None, count=1):
            child_env = dict(env, GHOSTERM_APP_TEST_FILTER=selected)
            command = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-pane; shift; exec "$gt_timeout_bin" --kill-after=8 350 "$@"',
                       'plugin-pane', str(ROOT), 'bash', 'scripts/apple-app-gate.sh']
            child = subprocess.Popen(command, cwd=ROOT, env=child_env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                output, _ = child.communicate(timeout=360)
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGTERM)
                    try:
                        child.communicate(timeout=8)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                        child.communicate()
            log = (ROOT / 'tmp/apple-app-test.log').read_text()
            (work / (label + '.log')).write_bytes(output + log.encode())
            if expected_failure:
                failures = re.findall(r'^.*: error: .* : (?:XCTAssert\w+ failed|failed -)[^\n]*', log, re.M)
                assert child.returncode == 1 and len(failures) == 1 and expected_failure in failures[0], (label, log[-12000:])
                assert 'Executed 1 test, with 1 failure (0 unexpected)' in log, (label, log[-12000:])
                assert not any(s in log for s in ['unexpected signal', 'AddressSanitizer', 'fatalError']), label
            else:
                assert child.returncode == 0 and f'Executed {count} test' + ('s' if count != 1 else '') + ', with 0 failures' in log, (label, output.decode()[-4000:], log[-12000:])
                assert 'skipped' not in log.lower(), label
            commands.append(dict(label=label, selected=selected, count=count, status=child.returncode,
                                 gateSHA256=hashlib.sha256(output).hexdigest(),
                                 xctestSHA256=hashlib.sha256(log.encode()).hexdigest()))
            print('PASS ' + label, flush=True)

        run('baseline', selected=baseline, count=baseline_count)
        linked = {name: sha(ROOT / name) for name in [
            'core/build/libghosterm_core.a', 'core/build/libghosterm_quickjs.a',
            'build/host-native/lib/libssh2.a',
        ]}
        run('html-lifetime-baseline', selected='PluginResourceUITests|PluginHTMLImageBudgetTests', count=28)
        for recipe in cases if args.mutate else []:
            label, old, new, expected, *selected_case = recipe
            if args.mutation_case and label not in args.mutation_case:
                continue
            selected = selected_case[0] if selected_case else selected_markdown
            filename = selected_case[1] if len(selected_case) > 1 else PANE
            path = source / filename
            original = originals[filename]
            stem = Path(filename).stem
            objects = {p: sha(p) for p in (source / 'apple/GhostermApp/.build').rglob('*') if p.name in (stem + '.swift.o', stem + '.o')}
            assert objects, 'compiled production pane object required'
            path.write_bytes(original.replace(old.encode(), new.encode()))
            try:
                run(label + '-assertion', selected=selected, expected_failure=expected)
                assert any(sha(p) != digest for p, digest in objects.items()), 'mutation must be compiled'
                run(label + '-control', CONTROL)
            finally:
                path.write_bytes(original)
            run(label + '-restored', selected=selected)
            assert sha(path) == inputs[filename], 'restoration must match exact source SHA256'
            groups.append(label)
        binary = next((source / 'apple/GhostermApp/.build').rglob('GhostermAppTests.xctest/Contents/MacOS/GhostermAppTests'))
        binary_sha = sha(binary)
    assert all(sha(ROOT / name) == digest for name, digest in inputs.items()), 'source changed during verification'
    assert all(sha(ROOT / name) == digest for name, digest in linked.items()), 'linked archive changed during verification'
    assert sha(gterm) == executable_sha, 'gterm changed during verification'
    assert all((ROOT / name).read_bytes() == value for name, value in originals.items()), 'original checkout changed'
    receipt = dict(schemaVersion=1, scope='macOS Native controls; injected directory chooser; owned SSH; Markdown/PNG/HTML/static raster budgets', routes=['direct', 'jump'] if args.route == 'both' else [args.route], baselineTests=baseline_count, resourceTests=28,
                   inputs=inputs, linkedArchives=linked, gtermSHA256=executable_sha, xctestSHA256=binary_sha,
                   commands=commands, mutations=groups)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + '\n')
    print(f'PLUGIN PANE PASS: {baseline_count} flows ({args.route}), {len(groups)} assertion/control/restore groups; receipt {out}', flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame):
        # Raise through the active child's finally, mutation restoration and
        # TemporaryDirectory cleanup; default SIGTERM would bypass all three.
        raise SystemExit(128 + number)

    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, interrupted)
    main()
