#!/usr/bin/env python3
"""Compile isolated release verifier mutations and error-path probes.

Only the raw C primitive is covered. Android policy/UI tests are separate.
Native OpenSSL (pkg-config libcrypto) is explicit; no host discovery in Android.
"""
import base64
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'core/src/extension/release-verify.c'
VECTOR = ROOT / 'android/app/src/androidTest/assets/plugin-release/vector.json'


def main():
    (ROOT / 'tmp').mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'))
    originals = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in [SOURCE, VECTOR]}
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libcrypto'], text=True, env=env))
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    if os.uname().sysname == 'Darwin':
        sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True, env=env).strip()
        compiler += ['-isysroot', sdk]
    fixture = json.loads(VECTOR.read_bytes())
    policy = json.loads(fixture['policy'])
    arrays = {'key': base64.b64decode(policy['keys'][0]['publicKey']),
              'signature': base64.b64decode(fixture['signature']), 'descriptor': fixture['descriptor'].encode()}
    declarations = '\n'.join('static unsigned char %s[] = {%s};' % (key, ','.join(map(str, data))) for key, data in arrays.items())
    probe = '''#include "ghosterm.h"
#include "extension/memory.h"
#include <stdio.h>
#include <string.h>
''' + declarations + '''
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int expected = GT_OK; size_t count = sizeof(descriptor);
    if (!strcmp(argv[1], "budget")) { gt__plugin_memory_limit(1); expected = GT_ERR_INTERNAL; }
    if (!strcmp(argv[1], "mismatch")) { signature[63] ^= 1; expected = GT_ERR_AUTH; }
    else if (!strcmp(argv[1], "argument")) { count = 0; expected = GT_ERR_INVALID_ARG; }
    else if (!strcmp(argv[1], "limit")) {
        static unsigned char large[65537];
        int value = gt_ext_release_verify(key, 32, signature, 64, large, sizeof(large));
        if (value == GT_ERR_INVALID_ARG) return 0;
        fprintf(stderr, "ASSERT release limit expected=%d actual=%d\\n", GT_ERR_INVALID_ARG, value); return 1;
    } else if (!strcmp(argv[1], "disabled")) expected = GT_ERR_UNSUPPORTED;
    else if (!strcmp(argv[1], "fault")) expected = GT_ERR_INTERNAL;
    else if (strcmp(argv[1], "valid") && strcmp(argv[1], "budget") && strcmp(argv[1], "released")) return 2;
    int value = gt_ext_release_verify(key, 32, signature, 64, descriptor, count);
    if (!strcmp(argv[1], "released") && gt__plugin_memory_info().used != 0) {
        fprintf(stderr, "ASSERT release allocation was not uncharged\\n"); return 1;
    }
    if (value == expected) return 0;
    fprintf(stderr, "ASSERT release %s expected=%d actual=%d\\n", argv[1], expected, value);
    return 1;
}
'''
    source = SOURCE.read_text()
    def replaced(old, new):
        if source.count(old) != 1: raise RuntimeError('mutation anchor mismatch: ' + old)
        return source.replace(old, new)
    cases = [
        ('shared-budget', replaced('gt__plugin_memory_alloc(NULL, size)', 'malloc(size)'), True, 'budget'),
        ('release-charge', replaced('gt__plugin_memory_free(NULL, message, size);', 'free(message);'), True, 'released'),
        ('domain', replaced('GHOSTERM-PLUGIN-RELEASE-V1', 'GHOSTERM-PLUGIN-RELEASE-V2'), True, 'valid'),
        ('separator', replaced('memcpy(message, domain, domain_size);', "memcpy(message, domain, domain_size); message[domain_size - 1] = 'x';"), True, 'valid'),
        ('payload', replaced('memcpy(message + domain_size, descriptor, descriptor_size);', 'memset(message + domain_size, 0, descriptor_size);'), True, 'valid'),
        ('accept-mismatch', replaced('checked == 0 ? GT_ERR_AUTH', 'checked == 0 ? GT_OK'), True, 'mismatch'),
        ('descriptor-limit', replaced('descriptor_size > GT_EXT_RELEASE_DESCRIPTOR_MAX', 'descriptor_size > GT_EXT_RELEASE_DESCRIPTOR_MAX + 1'), True, 'limit'),
        ('disabled-success', replaced('return GT_ERR_UNSUPPORTED;', 'return GT_OK;'), False, 'disabled'),
        ('unknown-as-denied', replaced('gt_status_t result = GT_ERR_INTERNAL;', 'gt_status_t result = GT_ERR_AUTH;').replace('EVP_MD_CTX *context = EVP_MD_CTX_new();', 'EVP_MD_CTX *context = NULL;'), True, 'fault'),
    ]
    faults = [
        ('message-allocation', replaced('unsigned char *message = gt__plugin_memory_alloc(NULL, size);', 'unsigned char *message = NULL;')),
        ('public-key-allocation', replaced('EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key, public_key_size)', 'NULL')),
        ('context-allocation', replaced('EVP_MD_CTX *context = EVP_MD_CTX_new();', 'EVP_MD_CTX *context = NULL;')),
        ('verification-incomplete', replaced('int checked = EVP_DigestVerify(context, signature, signature_size, message, size);', 'int checked = -1; (void)size;')),
    ]
    with tempfile.TemporaryDirectory(prefix='release-signature-', dir=ROOT / 'tmp') as directory:
        work = Path(directory); (work / 'probe.c').write_text(probe)
        def compile_source(code, enabled=True):
            (work / 'release.c').write_text(code)
            command = compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'core/include'), '-I', str(ROOT / 'core/src'), '-pthread']
            if enabled: command += ['-DGT_HAVE_RELEASE_SIGNATURE=1']
            command += [str(work / 'release.c'), str(work / 'probe.c'), str(ROOT / 'core/src/extension/memory.c'), str(ROOT / 'core/src/base/secure-random.c'), '-o', str(work / 'probe')] + flags
            result = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True)
            if result.returncode: raise RuntimeError('compile failure (not assertion red):\n' + result.stderr)
        def run(mode, red=False):
            result = subprocess.run([work / 'probe', mode], cwd=work, env=env, capture_output=True, text=True)
            if red:
                if result.returncode != 1 or result.stderr.count('ASSERT release ') != 1 or result.stdout:
                    raise RuntimeError('not one normal assertion red: ' + repr(result))
            elif result.returncode != 0 or result.stderr or result.stdout:
                raise RuntimeError('expected green: ' + repr(result))
        compile_source(source)
        for mode in ['valid', 'mismatch', 'argument', 'limit', 'released', 'budget']: run(mode)
        compile_source(source, False); run('disabled'); run('argument')
        print('baseline: Node public vector / mismatch / bounds / disabled PASS', flush=True)
        for name, code, enabled, target in cases:
            compile_source(code, enabled); run(target, red=True); run('argument')
            # Restore the same scenario. For the fault classification mutant,
            # preserve its failed context while restoring the status mapping.
            restored = code.replace('gt_status_t result = GT_ERR_AUTH;', 'gt_status_t result = GT_ERR_INTERNAL;') if target == 'fault' else source
            compile_source(restored, enabled); run(target); run('argument')
            print(name + ': compiled assertion RED / argument control GREEN / restored GREEN', flush=True)
        for name, code in faults:
            compile_source(code); run('fault'); run('argument')
            print(name + ': explicit INTERNAL / invalid-argument control PASS', flush=True)
    for path, digest in originals.items():
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest: raise RuntimeError('source changed: ' + str(path))
        print(str(path.relative_to(ROOT)) + ' SHA256 ' + digest, flush=True)
    print('release signature: 9 mutations / 4 error paths PASS; isolated files removed', flush=True)


if __name__ == '__main__':
    main()
