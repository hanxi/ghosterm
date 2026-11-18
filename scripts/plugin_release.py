"""Candidate offline release authorization. Never installs or activates plugins.

Only public release material is staged. Signing is delegated to an explicitly
selected host program; this module never reads or exports a private key.
"""
import base64
import binascii
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import stat
import struct
import subprocess
import tempfile
import time
from types import SimpleNamespace
import unicodedata
import zipfile
import zlib

DOMAIN = b'GHOSTERM-PLUGIN-RELEASE-V1\0'
ARCHIVE_LIMIT = 75497472
MEMBER_LIMIT = 16777216
TOTAL_LIMIT = 67108864


def public_json(api, raw):
    # Reuse bounded UTF-8/depth/duplicate validation, then retain mathematical
    # integer semantics before binary-float rounding can change signed policy.
    api.decode(raw)
    def number(token):
        value = Decimal(token)
        api.require(value.is_finite() and abs(value) <= ARCHIVE_LIMIT and value == value.to_integral_value(),
                    'RELEASE_JSON_NUMBER')
        return int(value)
    return json.loads(raw, parse_float=number)


def stable_file(api, source, limit, destination=None):
    fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    output = None
    try:
        before = os.fstat(fd)
        api.require(stat.S_ISREG(before.st_mode) and 0 <= before.st_size <= limit, 'RELEASE_FILE_LIMIT')
        if destination is not None:
            output = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
        pieces, count, digest = [], 0, hashlib.sha256()
        while True:
            data = os.read(fd, 65536)
            if not data:
                break
            count += len(data)
            api.require(count <= limit, 'RELEASE_FILE_LIMIT')
            digest.update(data)
            if output is None:
                pieces.append(data)
            else:
                at = 0
                while at < len(data):
                    at += os.write(output, data[at:])
        after = os.fstat(fd)
        api.require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                    (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns) and count == before.st_size,
                    'RELEASE_SOURCE_CHANGED')
        return b''.join(pieces) if output is None else (count, digest.hexdigest())
    finally:
        if output is not None:
            os.close(output)
        os.close(fd)


def digest_value(api, value):
    api.require(isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value), 'RELEASE_DIGEST')
    return value


def public_key(api, value):
    api.require(isinstance(value, str), 'RELEASE_PUBLIC_KEY')
    try:
        raw = base64.b64decode(value, validate=True)
    except (ValueError, binascii.Error):
        raise api.Failure('RELEASE_PUBLIC_KEY') from None
    api.require(len(raw) == 32 and base64.b64encode(raw).decode() == value, 'RELEASE_PUBLIC_KEY')
    return raw


def text(api, value, limit, code):
    api.require(isinstance(value, str) and 0 < len(value) <= limit and
                value == unicodedata.normalize('NFC', value) and
                not any(ord(c) < 32 or ord(c) == 127 for c in value), code)
    return value


def policy(api, value):
    api.require(isinstance(value, dict) and set(value) == {'schemaVersion', 'channel', 'keys', 'revokedKeyIds'} and
                type(value['schemaVersion']) in (int, float) and value['schemaVersion'] == 1, 'RELEASE_POLICY')
    api.require(isinstance(value['channel'], str) and re.fullmatch('[a-z0-9][a-z0-9.-]{0,63}', value['channel']), 'RELEASE_CHANNEL')
    api.require(isinstance(value['keys'], list) and 1 <= len(value['keys']) <= 64 and
                isinstance(value['revokedKeyIds'], list) and len(value['revokedKeyIds']) <= 256, 'RELEASE_POLICY')
    keys = {}
    for key in value['keys']:
        api.require(isinstance(key, dict) and set(key) == {'id', 'algorithm', 'publicKey', 'publisher', 'pluginIds'} and
                    key['algorithm'] == 'ed25519', 'RELEASE_POLICY_KEY')
        identifier = digest_value(api, key['id'])
        raw = public_key(api, key['publicKey'])
        api.require(identifier == api.sha(raw) and identifier not in keys, 'RELEASE_KEY_ID')
        text(api, key['publisher'], 128, 'RELEASE_PUBLISHER')
        api.require(isinstance(key['pluginIds'], list) and 1 <= len(key['pluginIds']) <= 64, 'RELEASE_PLUGIN_SCOPE')
        for plugin in key['pluginIds']:
            api.require(isinstance(plugin, str) and re.fullmatch('[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+', plugin) and len(plugin) <= 128,
                        'RELEASE_PLUGIN_SCOPE')
        api.require(len(set(key['pluginIds'])) == len(key['pluginIds']), 'RELEASE_PLUGIN_SCOPE')
        keys[identifier] = key
    revoked = [digest_value(api, item) for item in value['revokedKeyIds']]
    api.require(len(set(revoked)) == len(revoked), 'RELEASE_REVOKED_KEYS')
    return keys


def descriptor(api, value):
    api.require(isinstance(value, dict) and set(value) == {'schemaVersion', 'channel', 'keyId', 'pluginId', 'pluginVersion',
                                                        'publisher', 'packageSHA256', 'artifact'} and
                type(value['schemaVersion']) in (int, float) and value['schemaVersion'] == 1, 'RELEASE_DESCRIPTOR')
    digest_value(api, value['keyId']); digest_value(api, value['packageSHA256'])
    api.require(isinstance(value['channel'], str) and re.fullmatch('[a-z0-9][a-z0-9.-]{0,63}', value['channel']), 'RELEASE_CHANNEL')
    text(api, value['pluginId'], 128, 'RELEASE_PLUGIN_ID')
    api.require(re.fullmatch('[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+', value['pluginId']), 'RELEASE_PLUGIN_ID')
    text(api, value['pluginVersion'], 128, 'RELEASE_PLUGIN_VERSION')
    text(api, value['publisher'], 128, 'RELEASE_PUBLISHER')
    artifact = value['artifact']
    api.require(isinstance(artifact, dict) and set(artifact) == {'format', 'bytes', 'sha256'} and artifact['format'] == 'zip-v1' and
                type(artifact['bytes']) in (int, float) and artifact['bytes'] == int(artifact['bytes']) and 22 <= artifact['bytes'] <= ARCHIVE_LIMIT, 'RELEASE_ARTIFACT')
    digest_value(api, artifact['sha256'])
    return value


def authorize(api, trusted, keys, release):
    api.require(release['channel'] == trusted['channel'], 'RELEASE_CHANNEL_MISMATCH', 6)
    key = keys.get(release['keyId'])
    api.require(key is not None, 'RELEASE_UNTRUSTED_KEY', 6)
    api.require(release['keyId'] not in trusted['revokedKeyIds'], 'RELEASE_KEY_REVOKED', 6)
    api.require(release['publisher'] == key['publisher'] and release['pluginId'] in key['pluginIds'], 'RELEASE_KEY_SCOPE', 6)
    return key


def verify_signature(api, work, key, payload, signature, *, kind='release'):
    api.require(len(payload) <= 65536 and len(signature) == 64, 'RELEASE_SIGNATURE_SIZE')
    request = work / 'crypto-request.json'
    request.write_bytes(api.encoded(dict(kind=kind, publicKey=key['publicKey'], payload=base64.b64encode(payload).decode(),
                                        signature=base64.b64encode(signature).decode())))
    result = api.decode(api.run(['node', api.ROOT / 'scripts/plugin-release-crypto.mjs', request], 10))
    api.require(isinstance(result, dict) and set(result) == {'valid'} and result['valid'] is True, 'RELEASE_SIGNATURE', 6)


def schema_check(api, work, tools, trusted, release=None):
    request = work / 'schema-request.json'
    request.write_bytes(api.encoded(dict(tools=str(tools), policy=trusted, descriptor=release)))
    result = api.decode(api.run(['node', api.ROOT / 'scripts/plugin-release-schemas.mjs', request], 10))
    api.require(isinstance(result, dict) and set(result) == {'valid'} and result['valid'] is True, 'RELEASE_SCHEMA')


def unpack(api, archive, destination, size):
    """ZIP32, no extras/descriptors/gaps; stored and raw DEFLATE are bounded.

    Local headers, central entries, CRC, compressed EOF and complete byte ranges
    are all checked. Never use extractall or follow archive-selected links.
    """
    destination.mkdir()
    with archive.open('rb') as physical:
        physical.seek(-22, os.SEEK_END)
        tail = physical.read(22)
        magic, disk, central_disk, disk_count, count, central_size, central_start, comment = struct.unpack('<4s4H2IH', tail)
        api.require(magic == b'PK\x05\x06' and disk == central_disk == comment == 0 and disk_count == count and
                    1 <= count <= 1024 and count * 46 <= central_size <= count * (46 + 4096) and
                    central_start + central_size == size - 22, 'RELEASE_ZIP_DIRECTORY')
        physical.seek(central_start)
        infos = []
        for _ in range(count):
            record = physical.read(46)
            api.require(len(record) == 46, 'RELEASE_ZIP_DIRECTORY')
            fields = struct.unpack('<4s6H3I5H2I', record)
            mark, _made, _needed, flags, method, _time, _date, crc, compressed, original, namesize, extrasize, commentsize, diskstart, _internal, external, offset = fields
            api.require(mark == b'PK\x01\x02' and 0 < namesize <= 4096 and extrasize == commentsize == diskstart == 0,
                        'RELEASE_ZIP_DIRECTORY')
            raw_name = physical.read(namesize)
            api.require(len(raw_name) == namesize, 'RELEASE_ZIP_DIRECTORY')
            name = raw_name.decode('utf-8' if flags & 0x800 else 'ascii', errors='strict')
            infos.append(SimpleNamespace(filename=name, flag_bits=flags, compress_type=method, CRC=crc,
                                         compress_size=compressed, file_size=original, external_attr=external,
                                         header_offset=offset))
        api.require(physical.tell() == size - 22, 'RELEASE_ZIP_DIRECTORY')
        folded, total, cursor = set(), 0, 0
        for item in sorted(infos, key=lambda info: info.header_offset):
            name = api.member(item.filename)
            api.require(name.lower() not in folded and not item.external_attr & 16 and
                        stat.S_IFMT(item.external_attr >> 16) in (0, stat.S_IFREG), 'RELEASE_ZIP_MEMBER')
            folded.add(name.lower())
            api.require(item.flag_bits & ~0x800 == 0 and item.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED) and
                        0 <= item.file_size <= MEMBER_LIMIT and 0 <= item.compress_size <= ARCHIVE_LIMIT and item.header_offset == cursor,
                        'RELEASE_ZIP_LAYOUT')
            total += item.file_size
            api.require(total <= TOTAL_LIMIT, 'RELEASE_ZIP_BUDGET')
            physical.seek(cursor)
            header = physical.read(30)
            api.require(len(header) == 30, 'RELEASE_ZIP_HEADER')
            mark, _version, flags, method, _time, _date, crc, compressed, original, namesize, extrasize = struct.unpack('<4s5H3I2H', header)
            name_bytes = name.encode('utf-8' if flags & 0x800 else 'ascii')
            api.require(mark == b'PK\x03\x04' and flags == item.flag_bits and method == item.compress_type and
                        (crc, compressed, original) == (item.CRC, item.compress_size, item.file_size) and extrasize == 0 and
                        namesize == len(name_bytes) and physical.read(namesize) == name_bytes,
                        'RELEASE_ZIP_HEADER')
            cursor += 30 + namesize + compressed
            api.require(cursor <= central_start, 'RELEASE_ZIP_LAYOUT')
            target = destination / name
            target.parent.mkdir(parents=True, exist_ok=True)
            decoder = zlib.decompressobj(-15) if method == zipfile.ZIP_DEFLATED else None
            remaining, produced, checksum = compressed, 0, 0
            with target.open('xb') as output:
                while remaining:
                    chunk = physical.read(min(65536, remaining))
                    api.require(bool(chunk), 'RELEASE_ZIP_TRUNCATED')
                    remaining -= len(chunk)
                    pending = chunk
                    while pending:
                        data = decoder.decompress(pending, min(65536, original - produced + 1)) if decoder else pending
                        pending = decoder.unconsumed_tail if decoder else b''
                        produced += len(data)
                        api.require(produced <= original, 'RELEASE_ZIP_BUDGET')
                        checksum = binascii.crc32(data, checksum)
                        output.write(data)
                    if decoder:
                        api.require(not decoder.unused_data and (not decoder.eof or remaining == 0), 'RELEASE_ZIP_STREAM')
                api.require(produced == original and checksum == crc and (decoder is None or decoder.eof), 'RELEASE_ZIP_CONTENT')
        api.require(cursor == central_start, 'RELEASE_ZIP_LAYOUT')


def signer(api, work, config, message):
    api.require(isinstance(config, dict) and set(config) <= {'executable', 'sha256', 'prefixArguments', 'environmentNames'} and
                {'executable', 'sha256', 'prefixArguments'} <= set(config), 'RELEASE_SIGNER_CONFIG')
    executable = Path(config['executable'])
    api.require(executable.is_absolute() and executable.resolve(strict=True) == executable and os.access(executable, os.X_OK), 'RELEASE_SIGNER_EXECUTABLE')
    expected = digest_value(api, config['sha256'])
    api.require(api.sha(stable_file(api, executable, 268435456)) == expected, 'RELEASE_SIGNER_DIGEST')
    arguments = config['prefixArguments']; names = config.get('environmentNames', [])
    api.require(isinstance(arguments, list) and len(arguments) <= 32 and all(isinstance(a, str) and '\0' not in a and len(a.encode()) <= 4096 for a in arguments), 'RELEASE_SIGNER_ARGUMENTS')
    api.require(isinstance(names, list) and len(names) <= 32 and all(isinstance(n, str) and re.fullmatch('[A-Za-z_][A-Za-z0-9_]{0,63}', n) for n in names) and
                len(names) == len(set(names)) and all(n in os.environ for n in names), 'RELEASE_SIGNER_ENVIRONMENT')
    environment = {name: os.environ[name] for name in names}
    environment['TMPDIR'] = str(api.TMP)
    # Resolve the shared host timeout before dropping the caller environment.
    timeout = api.run(['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-release; command -v "$gt_timeout_bin"',
                       'plugin-release', api.ROOT], 5).decode().strip()
    api.require(Path(timeout).is_absolute(), 'RELEASE_TIMEOUT_TOOL')
    command = [timeout, '--kill-after=3', '15', str(executable), *arguments]
    child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             env=environment, cwd=work, start_new_session=True)
    try:
        signature = bytearray()
        written = diagnostics = 0
        deadline = time.monotonic() + 20
        with selectors.DefaultSelector() as selector:
            for stream, event in [(child.stdin, selectors.EVENT_WRITE), (child.stdout, selectors.EVENT_READ), (child.stderr, selectors.EVENT_READ)]:
                os.set_blocking(stream.fileno(), False)
                selector.register(stream, event)
            while selector.get_map():
                api.require(time.monotonic() < deadline, 'RELEASE_SIGNER_TIMEOUT')
                for event, _ in selector.select(.1):
                    stream = event.fileobj
                    if stream is child.stdin:
                        try:
                            written += os.write(stream.fileno(), message[written:written + 65536])
                        except BrokenPipeError:
                            written = len(message)
                        if written == len(message):
                            selector.unregister(stream); stream.close()
                    else:
                        data = os.read(stream.fileno(), 4096)
                        if not data:
                            selector.unregister(stream); stream.close()
                        elif stream is child.stdout:
                            signature.extend(data)
                            api.require(len(signature) <= 64, 'RELEASE_SIGNER_OUTPUT')
                        else:
                            diagnostics += len(data)
                            api.require(diagnostics <= 4096, 'RELEASE_SIGNER_DIAGNOSTICS')
        child.wait(timeout=2)
    finally:
        if child.poll() is None:
            try:
                os.killpg(child.pid, signal.SIGTERM)
                child.wait(timeout=3)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait()
        for stream in (child.stdin, child.stdout, child.stderr):
            stream.close()
    api.require(child.returncode == 0 and len(signature) == 64, 'RELEASE_SIGNER_FAILED')
    api.require(api.sha(stable_file(api, executable, 268435456)) == expected, 'RELEASE_SIGNER_CHANGED')
    return bytes(signature)


def execute(args, api):
    if args.release_action == 'verify-policy':
        return verify_channel_policy(args, api)
    try:
        return checked_release(args, api)
    except (zipfile.BadZipFile, struct.error, zlib.error):
        raise api.Failure('RELEASE_ZIP_CONTENT') from None


def verify_channel_policy(args, api):
    public_key(api, args.channel_root)
    statement_bytes = stable_file(api, args.statement, 65536)
    signature = stable_file(api, args.signature, 64)
    policy_bytes = stable_file(api, args.policy, 131072)
    statement = public_json(api, statement_bytes)
    api.require(isinstance(statement, dict) and set(statement) == {
        'schemaVersion', 'channel', 'generation', 'issuedAt', 'expiresAt', 'policySHA256'
    } and type(statement['schemaVersion']) is int and statement['schemaVersion'] == 1, 'CHANNEL_POLICY_STATEMENT')

    def positive_decimal(value):
        api.require(isinstance(value, str) and re.fullmatch('[1-9][0-9]{0,19}', value) and
                    int(value) <= (1 << 64) - 1, 'CHANNEL_POLICY_INTEGER')
        return int(value)

    positive_decimal(statement['generation'])
    issued = positive_decimal(statement['issuedAt'])
    expires = positive_decimal(statement['expiresAt'])
    with tempfile.TemporaryDirectory(prefix='channel-policy-', dir=api.TMP) as temporary:
        verify_signature(api, Path(temporary), {'publicKey': args.channel_root}, statement_bytes, signature,
                         kind='channel-policy')
    trusted = public_json(api, policy_bytes)
    policy(api, trusted)
    api.require(statement['channel'] == trusted['channel'] == args.channel, 'RELEASE_CHANNEL_MISMATCH', 6)
    api.require(digest_value(api, statement['policySHA256']) == api.sha(policy_bytes), 'CHANNEL_POLICY_DIGEST', 6)
    api.require(issued <= int(time.time()) < expires, 'CHANNEL_POLICY_FRESHNESS', 6)
    return dict(kind='verified-channel-policy', channel=args.channel, generation=statement['generation'],
                expiresAt=statement['expiresAt'], policySHA256=statement['policySHA256'],
                statementSHA256=api.sha(statement_bytes)), 0


def checked_release(args, api):
    if args.release_action == 'sign':
        api.require(not args.out.exists() and not args.out.is_symlink(), 'OUTPUT_EXISTS')
    tools = api.tools_path(args.tools)
    policy_bytes = stable_file(api, args.policy, 131072)
    trusted = public_json(api, policy_bytes)
    keys = policy(api, trusted)
    with tempfile.TemporaryDirectory(prefix='plugin-release-', dir=api.TMP) as temporary:
        work = Path(temporary)
        archive = work / 'plugin.zip'
        size, digest = stable_file(api, args.artifact, ARCHIVE_LIMIT, archive)
        schema_check(api, work, tools, trusted)
        host = api.read_json(args.host_json) if args.host_json else None
        payload = signature = release = key = None
        if args.release_action == 'verify':
            payload = stable_file(api, args.descriptor, 65536)
            release = descriptor(api, public_json(api, payload))
            schema_check(api, work, tools, trusted, release)
            key = authorize(api, trusted, keys, release)
            signature = stable_file(api, args.signature, 64)
            verify_signature(api, work, key, payload, signature)
            api.require((release['artifact']['bytes'], release['artifact']['sha256']) == (size, digest), 'RELEASE_ARTIFACT_MISMATCH', 6)
        package = work / 'package'
        unpack(api, archive, package, size)
        report = api.worker(work, tools, package, host=host)
        api.require(host is None or report.get('exitCode', 0) == 0, 'RELEASE_HOST_INCOMPATIBLE', 4)
        manifest = api.decode((package / 'plugin.json').read_bytes())
        identity = dict(pluginId=manifest['id'], pluginVersion=manifest['pluginVersion'], publisher=manifest['publisher'],
                        packageSHA256=api.sha((package / 'integrity.json').read_bytes()))
        if args.release_action == 'sign':
            release = dict(schemaVersion=1, channel=trusted['channel'], keyId=args.key_id, **identity,
                           artifact=dict(format='zip-v1', bytes=size, sha256=digest))
            descriptor(api, release)
            schema_check(api, work, tools, trusted, release)
            key = authorize(api, trusted, keys, release)
            payload = api.encoded(release) + b'\n'
            signature = signer(api, work, api.read_json(args.signer_json), DOMAIN + payload)
            verify_signature(api, work, key, payload, signature)
        else:
            api.require(all(release[name] == value for name, value in identity.items()), 'RELEASE_IDENTITY_MISMATCH', 6)
        if args.release_action == 'sign':
            output = work / 'release'
            output.mkdir()
            (output / 'release.json').write_bytes(payload)
            (output / 'release.sig').write_bytes(signature)
            os.rename(archive, output / 'plugin.zip')
            api.publish(output, args.out)
        return dict(kind='release-signed' if args.release_action == 'sign' else 'release-verified',
                    channel=release['channel'], keyId=release['keyId'], **identity,
                    artifactSHA256=digest, artifactBytes=size, publisherAuthenticated=True,
                    policySHA256=api.sha(policy_bytes), trustSource='explicit-policy-snapshot',
                    authorization='not-granted', installation=False, compatibility=report['compatibility']), 0
