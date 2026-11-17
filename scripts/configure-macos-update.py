#!/usr/bin/env python3
"""Embed the public update key; private signing material never enters the app."""
import base64
import os
from pathlib import Path
import plistlib
import sys


def configure(path, channel, key):
    if channel not in ('dev', 'release'):
        raise ValueError('unknown update channel')
    plist = plistlib.loads(path.read_bytes())
    if key:
        if len(base64.b64decode(key, validate=True)) != 32:
            raise ValueError('GHOSTERM_UPDATE_PUBLIC_KEY must be a 32-byte Ed25519 public key in base64')
        plist['SUPublicEDKey'] = key
    plist['GhostermUpdateChannel'] = channel
    plist['SUFeedURL'] = ('https://github.com/hanxi/ghosterm/releases/download/dev/appcast.xml'
                          if channel == 'dev' else
                          'https://github.com/hanxi/ghosterm/releases/latest/download/appcast.xml')
    path.write_bytes(plistlib.dumps(plist, sort_keys=False))
    print('Update signing:', 'configured' if key else 'not configured; manual download only')


if __name__ == '__main__':
    configure(Path(sys.argv[1]), sys.argv[2], os.environ.get('GHOSTERM_UPDATE_PUBLIC_KEY', ''))
