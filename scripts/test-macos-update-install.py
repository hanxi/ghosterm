#!/usr/bin/env python3
"""Install/relaunch only a disposable ad-hoc app via real Sparkle and loopback HTTP.

No terminal, user app, account credentials or external host is used.
"""
import argparse
import functools
import http.server
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import tempfile
import threading
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
ARTIFACT = ROOT / 'apple/GhostermApp/.build/artifacts/sparkle/Sparkle'
FRAMEWORK = ARTIFACT / 'Sparkle.xcframework/macos-arm64_x86_64/Sparkle.framework'
NS = 'http://www.andymatuschak.org/xml-namespaces/sparkle'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=['install', 'later', 'cancel-quit', 'bad-feed', 'bad-archive'], action='append')
    args = parser.parse_args()
    cases = args.case or ['install', 'later', 'cancel-quit', 'bad-feed', 'bad-archive']
    with tempfile.TemporaryDirectory(prefix='update-install-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        env = dict(os.environ, TMPDIR=str(work))
        keys = json.loads(subprocess.check_output(['swift', '-e', '''
import CryptoKit
import Foundation
let k = Curve25519.Signing.PrivateKey()
print(String(data: try! JSONSerialization.data(withJSONObject: ["private": k.rawRepresentation.base64EncodedString(), "public": k.publicKey.rawRepresentation.base64EncodedString()]), encoding: .utf8)!)
'''], env=env))
        binary = work / 'Probe'
        subprocess.run(['swiftc', str(ROOT / 'scripts/probes/MacUpdate.swift'), '-F', str(FRAMEWORK.parent),
                        '-framework', 'Sparkle', '-Xlinker', '-rpath', '-Xlinker', '@executable_path/../Frameworks',
                        '-o', str(binary)], check=True, env=env)
        class Handler(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *_): pass
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Handler, directory=str(work)))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            for case in cases:
                folder = work / case; folder.mkdir()
                app = folder / 'Ghosterm.app'
                macos = app / 'Contents/MacOS'; macos.mkdir(parents=True)
                frameworks = app / 'Contents/Frameworks'; frameworks.mkdir()
                shutil.copy2(binary, macos / 'Probe')
                subprocess.run(['ditto', str(FRAMEWORK), str(frameworks / 'Sparkle.framework')], check=True)
                info = dict(CFBundleIdentifier='dev.ghosterm.update-probe.' + folder.parent.name + '.' + case,
                    CFBundleName='Ghosterm Update Probe', CFBundleExecutable='Probe', CFBundlePackageType='APPL',
                    CFBundleVersion='200', CFBundleShortVersionString='1.2.0', LSMinimumSystemVersion='13.0',
                    SUFeedURL=f'http://127.0.0.1:{server.server_port}/{case}/appcast.xml', SUPublicEDKey=keys['public'],
                    SURequireSignedFeed=True, SUVerifyUpdateBeforeExtraction=True, SUEnableAutomaticChecks=False,
                    SUAutomaticallyUpdate=False, SUAllowsAutomaticUpdates=False, SUEnableSystemProfiling=False,
                    NSAppTransportSecurity=dict(NSAllowsArbitraryLoads=True), LSUIElement=True,
                    GhostermUpdateReceipt=str(folder / 'receipt.txt'), GhostermUpdateMode=case)
                plist = app / 'Contents/Info.plist'
                def write_app(version):
                    info['CFBundleVersion'] = version
                    plist.write_bytes(plistlib.dumps(info))
                    subprocess.run(['codesign', '--force', '--sign', '-', str(app)], check=True, capture_output=True)
                write_app('200')
                archive = folder / 'Ghosterm.zip'
                subprocess.run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', str(app), str(archive)], check=True)
                def sign(path, extra=()):
                    r = subprocess.run([str(ARTIFACT / 'bin/sign_update'), '--ed-key-file', '-', *extra, str(path)],
                        input=keys['private']+'\n', text=True, capture_output=True, env=env)
                    if r.returncode: raise RuntimeError('fixture signing failed')
                    return r.stdout.strip()
                signature = sign(archive, ['-p'])
                rss = ET.Element('rss', {'version': '2.0'}); channel = ET.SubElement(rss, 'channel')
                ET.SubElement(channel, 'title').text = 'Owned update fixture'
                item = ET.SubElement(channel, 'item')
                ET.SubElement(item, '{'+NS+'}version').text = '200'
                ET.SubElement(item, '{'+NS+'}shortVersionString').text = '1.2.0'
                ET.SubElement(item, 'enclosure', {'url':f'http://127.0.0.1:{server.server_port}/{case}/Ghosterm.zip',
                    'length':str(archive.stat().st_size), 'type':'application/octet-stream', '{'+NS+'}edSignature':signature})
                feed = folder / 'appcast.xml'
                ET.ElementTree(rss).write(feed, encoding='utf-8', xml_declaration=True)
                sign(feed)
                if case == 'bad-feed': feed.write_bytes(feed.read_bytes().replace(b'Owned', b'Other'))
                if case == 'bad-archive':
                    data = bytearray(archive.read_bytes()); data[len(data)//2] ^= 1; archive.write_bytes(data)
                write_app('100')
                record = folder / 'receipt.txt'
                runenv = dict(env, GHOSTERM_UPDATE_MODE=case, GHOSTERM_UPDATE_RECEIPT=str(record),
                              CFFIXED_USER_HOME=str(folder / 'home'))
                # Sparkle's system cache namespace is a uniquely owned symlink;
                # archive/helper bytes stay under the project temporary root.
                cache_link = Path.home() / 'Library/Caches' / info['CFBundleIdentifier']
                cache = folder / 'sparkle-cache'; cache.mkdir()
                assert not cache_link.exists() and not cache_link.is_symlink()
                cache_link.symlink_to(cache, target_is_directory=True)
                child = subprocess.Popen([str(macos / 'Probe')], env=runenv, stdout=subprocess.DEVNULL,
                    stderr=(folder / 'stderr.log').open('w'), start_new_session=True)
                try:
                    deadline = time.monotonic() + 60
                    text = ''
                    while time.monotonic() < deadline:
                        text = record.read_text() if record.exists() else ''
                        if 'relaunched-200' in text or 'ERROR' in text or 'later-kept-old' in text: break
                        time.sleep(0.1)
                    expected = 'ERROR' if case.startswith('bad-') else 'later-kept-old' if case == 'later' else 'relaunched-200'
                    assert expected in text, (case, text, (folder / 'stderr.log').read_text()[-5000:])
                    current = plistlib.loads(plist.read_bytes())['CFBundleVersion']
                    assert current == ('200' if case in ['install', 'cancel-quit'] else '100'), (case, current)
                    if case == 'cancel-quit': assert 'quit-cancelled-kept-old' in text, text
                    if case == 'bad-feed': assert 'downloading' not in text, text
                    if case == 'bad-archive': assert 'ready' not in text, text
                    print('PASS real Sparkle ad-hoc fixture: ' + case + '\n' + text, flush=True)
                finally:
                    if child.poll() is None: child.terminate()
                    try: child.wait(timeout=5)
                    except subprocess.TimeoutExpired: child.kill(); child.wait()
                    # Installation helpers may briefly outlive the host app.
                    # Match only this invocation's unique app/cache identity.
                    for _ in range(30):
                        processes = subprocess.check_output(['ps', '-axo', 'pid=,command='], text=True)
                        owned = []
                        for line in processes.splitlines():
                            parts = line.strip().split(None, 1)
                            if len(parts) == 2 and (str(folder) in parts[1] or info['CFBundleIdentifier'] in parts[1]) and any(
                                token in parts[1] for token in ['/Autoupdate', '/Updater.app/', '/Ghosterm.app/Contents/MacOS/Probe']):
                                owned.append(int(parts[0]))
                        if not owned: break
                        time.sleep(0.1)
                    for pid in owned:
                        try: os.kill(pid, signal.SIGTERM)
                        except ProcessLookupError: pass
                    if cache_link.is_symlink() and cache_link.resolve() == cache.resolve(): cache_link.unlink()
                    preference = Path.home() / 'Library/Preferences' / (info['CFBundleIdentifier'] + '.plist')
                    preference.unlink(missing_ok=True)
        finally:
            server.shutdown(); server.server_close()
    print('MACOS UPDATE INSTALL SMOKE PASS')


if __name__ == '__main__': main()
