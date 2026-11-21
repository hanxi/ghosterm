#!/usr/bin/env python3
"""Real Sparkle signing tests over disposable ZIPs, with ephemeral in-memory keys."""
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent.parent
(ROOT / 'tmp').mkdir(exist_ok=True)
requested_tmp = Path(os.environ.get('TMPDIR', ROOT / 'tmp')).resolve()
TEST_TMP = requested_tmp if requested_tmp.is_dir() and requested_tmp.is_relative_to(ROOT / 'tmp') else ROOT / 'tmp'
os.environ['TMPDIR'] = str(TEST_TMP)
spec = importlib.util.spec_from_file_location('publisher', ROOT / 'scripts/make-macos-update.py')
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)
SIGNER = ROOT / 'apple/GhostermApp/.build/artifacts/sparkle/Sparkle/bin/sign_update'


class PublisherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.keys = json.loads(subprocess.check_output(['swift', '-e', '''
import CryptoKit
import Foundation
let key = Curve25519.Signing.PrivateKey()
print(String(data: try! JSONSerialization.data(withJSONObject: [
 "private": key.rawRepresentation.base64EncodedString(),
 "public": key.publicKey.rawRepresentation.base64EncodedString()]), encoding: .utf8)!)
''']))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='update-publish-', dir=TEST_TMP)
        self.addCleanup(self.temp.cleanup)
        self.dist = Path(self.temp.name)

    def fixture(self, channel='dev', public=None, dirty=False, source_length=40):
        version = '1.2.0-dev' if channel == 'dev' else '1.2.0'
        info = dict(CFBundleShortVersionString=version, CFBundleVersion='1794879958',
                    GhostermUpdateChannel=channel, LSMinimumSystemVersion='13.0')
        if public:
            info['SUPublicEDKey'] = public
        receipt = f'''版本: {version} (build 42)
渠道: {channel}
构建时刻: 2026-11-17T01:45:58Z
架构: arm64
签名: adhoc
公证: 无
源码: git {'d' * source_length}{'+dirty' if dirty else ''}
'''
        with zipfile.ZipFile(self.dist / 'Ghosterm.zip', 'w') as archive:
            archive.writestr('Ghosterm.app/Contents/Info.plist', plistlib.dumps(info))
            archive.writestr('Ghosterm.app/Contents/Resources/BUILD-INFO.txt', receipt)
        (self.dist / 'Ghosterm.dmg').write_bytes(b'isolated distribution fixture')
        sums = [hashlib.sha256((self.dist / name).read_bytes()).hexdigest() + '  ' + name
                for name in ['Ghosterm.zip', 'Ghosterm.dmg']]
        (self.dist / 'SHA256SUMS').write_text('\n'.join(sums) + '\n')

    def publish(self, channel='dev', private=''):
        with patch.dict(os.environ, {'GHOSTERM_UPDATE_PRIVATE_KEY': private}):
            publisher.metadata(self.dist, channel, SIGNER, 'd'*40)
        return json.loads((self.dist / 'version.json').read_text())

    def verify_feed(self):
        result = subprocess.run([str(SIGNER), '--verify', '--ed-key-file', '-', str(self.dist / 'appcast.xml')],
            input=self.keys['private'] + '\n', text=True, capture_output=True)
        return result.returncode

    def testAdhocDevArchiveAndFeedHaveRealSignatures(self):
        self.fixture(public=self.keys['public'])
        manifest = self.publish(private=self.keys['private'])
        self.assertEqual(manifest['signature'], 'adhoc')
        self.assertFalse(manifest['notarized'])
        self.assertEqual(len(base64.b64decode(manifest['update_signature'])), 64)
        self.assertEqual(manifest['download_base'], 'https://github.com/hanxi/ghosterm/releases/download/dev')
        self.assertEqual(self.verify_feed(), 0)
        xml = ET.parse(self.dist / 'appcast.xml')
        self.assertEqual(xml.find('.//{' + publisher.NS + '}version').text, manifest['update_version'])
        feed = self.dist / 'appcast.xml'
        feed.write_bytes(feed.read_bytes().replace(b'Ghosterm dev', b'Ghosterm bad'))
        self.assertNotEqual(self.verify_feed(), 0, 'tampered feed must fail real Sparkle verification')

    def testStableReleaseUsesItsOwnTagAndReceipt(self):
        self.fixture(channel='release', public=self.keys['public'])
        manifest = self.publish(channel='release', private=self.keys['private'])
        self.assertEqual(manifest['download_base'], 'https://github.com/hanxi/ghosterm/releases/download/v1.2.0')
        self.assertEqual(self.verify_feed(), 0)

    def testNoKeysProducesExplicitManualOnlyManifestAndEmptyFeed(self):
        self.fixture()
        manifest = self.publish()
        self.assertIsNone(manifest['update_signature'])
        self.assertIsNone(ET.parse(self.dist / 'appcast.xml').find('.//item'))

    def testConfiguredPublicKeyWithoutPrivateKeyFailsClosed(self):
        self.fixture(public=self.keys['public'])
        with self.assertRaisesRegex(ValueError, 'both be configured'):
            self.publish()

    def testWrongPublicKeyCannotPublishSignedUpdate(self):
        self.fixture(public=base64.b64encode(b'x'*32).decode())
        with self.assertRaises(subprocess.CalledProcessError):
            self.publish(private=self.keys['private'])
        self.assertFalse((self.dist / 'version.json').exists())

    def testChecksumMismatchCannotPublish(self):
        self.fixture(public=self.keys['public'])
        (self.dist / 'Ghosterm.zip').write_bytes((self.dist / 'Ghosterm.zip').read_bytes() + b'changed')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.publish(private=self.keys['private'])

    def testDirtyOrWrongChannelCannotPublish(self):
        self.fixture(dirty=True)
        with self.assertRaisesRegex(ValueError, 'dirty'):
            self.publish()
        self.fixture()
        with self.assertRaisesRegex(ValueError, 'channel mismatch'):
            self.publish(channel='release')

    def testFormalUpdateRequiresFullSourceCommit(self):
        self.fixture(channel='release', source_length=12)
        with self.assertRaisesRegex(ValueError, 'does not match the commit'):
            self.publish(channel='release')


if __name__ == '__main__':
    unittest.main()
