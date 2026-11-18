#!/usr/bin/env python3
"""Embed the reviewed public plugin channel configuration in the App bundle."""
import base64
import os
from pathlib import Path
import plistlib
import re
import sys
from urllib.parse import urlsplit


def configure(path, channel, key, directory):
    if not any((channel, key, directory)):
        return
    if not re.fullmatch(r'[a-z0-9][a-z0-9.-]{0,63}', channel):
        raise ValueError('invalid GHOSTERM_PLUGIN_CHANNEL')
    if len(base64.b64decode(key, validate=True)) != 32:
        raise ValueError('GHOSTERM_PLUGIN_CHANNEL_ROOT must encode 32 public key bytes')
    if any(character.isspace() or ord(character) < 32 for character in directory) or '\\' in directory:
        raise ValueError('invalid GHOSTERM_PLUGIN_CHANNEL_URL')
    url = urlsplit(directory)
    if (url.scheme != 'https' or not url.hostname or url.username is not None or url.password is not None
            or '?' in directory or '#' in directory or url.port == 0):
        raise ValueError('GHOSTERM_PLUGIN_CHANNEL_URL must be a credential-free HTTPS directory')
    plist = plistlib.loads(path.read_bytes())
    plist['GhostermPluginChannel'] = {'Name': channel, 'Root': key, 'Directory': directory}
    path.write_bytes(plistlib.dumps(plist, sort_keys=False))


if __name__ == '__main__':
    configure(Path(sys.argv[1]), *(os.environ.get(name, '') for name in
                                   ('GHOSTERM_PLUGIN_CHANNEL', 'GHOSTERM_PLUGIN_CHANNEL_ROOT', 'GHOSTERM_PLUGIN_CHANNEL_URL')))
