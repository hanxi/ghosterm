#!/usr/bin/env python3
"""Embed the reviewed public plugin channel configuration in the App bundle."""
import argparse
import base64
import os
from pathlib import Path
import plistlib
import re
from urllib.parse import urlsplit


def channel_config(channel, key, directory):
    if not any((channel, key, directory)):
        return
    if not re.fullmatch(r'[a-z0-9][a-z0-9.-]{0,63}', channel):
        raise ValueError('invalid GHOSTERM_PLUGIN_CHANNEL')
    root = base64.b64decode(key, validate=True)
    if len(root) != 32 or base64.b64encode(root).decode('ascii') != key:
        raise ValueError('GHOSTERM_PLUGIN_CHANNEL_ROOT must encode 32 public key bytes in canonical Base64')
    if (any(character.isspace() or ord(character) < 32 or ord(character) == 127 for character in directory)
            or any(character in directory for character in '\\"<>^`{|}')
            or re.search(r'%(?![0-9A-Fa-f]{2})', directory)):
        raise ValueError('invalid GHOSTERM_PLUGIN_CHANNEL_URL')
    url = urlsplit(directory)
    if (url.scheme != 'https' or not url.hostname or url.username is not None or url.password is not None
            or '?' in directory or '#' in directory or url.netloc.endswith(':') or url.port == 0):
        raise ValueError('GHOSTERM_PLUGIN_CHANNEL_URL must be a credential-free HTTPS directory')
    return {'Name': channel, 'Root': key, 'Directory': directory}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('plist', nargs='?', type=Path)
    mode.add_argument('--check-required', action='store_true')
    args = parser.parse_args()
    try:
        config = channel_config(*(os.environ.get(name, '') for name in
                                  ('GHOSTERM_PLUGIN_CHANNEL', 'GHOSTERM_PLUGIN_CHANNEL_ROOT', 'GHOSTERM_PLUGIN_CHANNEL_URL')))
    except ValueError as error:
        parser.error(str(error))
    if args.check_required and config is None:
        parser.error('plugin runtime packaging requires channel configuration')
    if args.plist is not None and config is not None:
        plist = plistlib.loads(args.plist.read_bytes())
        plist['GhostermPluginChannel'] = config
        args.plist.write_bytes(plistlib.dumps(plist, sort_keys=False))
