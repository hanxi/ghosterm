#!/usr/bin/env python3
"""Credential UI regressions against real SSH, with same-APK configuration controls."""
import hashlib

MARK = 'GT_ANDROID_CREDENTIAL_MUTATION'


def execute(root, build, test):
    dialog = root/'android/app/src/main/java/com/hanxi/ghosterm/SSHConnectionDialog.kt'
    model = root/'android/app/src/main/java/com/hanxi/ghosterm/SessionsViewModel.kt'
    vault = root/'android/data/src/main/java/com/hanxi/ghosterm/data/IdentityVault.kt'
    core = root/'core/src/ssh.c'
    originals = {p: p.read_bytes() for p in (dialog, model, vault, core)}
    hashes = {p: hashlib.sha256(data).digest() for p, data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual credential mutation')
    encrypted = 'encryptedPrivateKeyRetriesWithFreshRequestAndCorrectPassphrase'
    password = 'passwordOnlyRetriesAndActivityRecreationClearsTypedPassword'
    cases = [
        ('key-retry', core, 'if(s->pem && rc==LIBSSH2_ERROR_FILE)error=GT_ERR_AUTH;',
         'if(false && s->pem && rc==LIBSSH2_ERROR_FILE)error=GT_ERR_AUTH;', encrypted),
        ('key-hint', vault, 'StoredIdentity(id,name.trim(),requirement == 1)',
         'StoredIdentity(id,name.trim(),false)', encrypted),
        ('passphrase', model, '.privateKey(prompt,checkNotNull(material),passphrase)',
         '.privateKey(prompt,checkNotNull(material),byteArrayOf())', encrypted),
        ('password', model, '.password(prompt,secret)', '.password(prompt,byteArrayOf())', password),
        ('saved-secret', dialog, 'var secret by remember(attempt.id, prompt?.request)',
         'var secret by androidx.compose.runtime.saveable.rememberSaveable(attempt.id, prompt?.request)', password),
        ('mode-secret', dialog, 'passwordMode = true; secret = ""', 'passwordMode = true;',
         'switchingKeyAndPasswordModesNeverCarriesTheOtherSecret'),
        ('late-secret', model, 'finally { secret.fill(0) }', 'finally { Unit }',
         'cancelledPasswordRequestErasesLateMaterialAndNewPromptStartsEmpty'),
    ]
    try:
        for label, path, old, new, method in cases:
            source = originals[path].decode()
            if source.count(old) != 1:
                raise RuntimeError('Ambiguous credential mutation ' + label)
            if label == 'saved-secret':
                # Persistence alone is still cleared by the existing disposal
                # hook. Remove that second guard to actually retain the secret.
                guard = 'onDispose { secret = "" }'
                if source.count(guard) != 1:
                    raise RuntimeError('Ambiguous secret disposal guard')
                source = source.replace(guard, 'onDispose { Unit }')
            path.write_text(source.replace(old, new)+'\n// '+MARK+'\n')
            label = 'credentials-'+label
            selection = 'com.hanxi.ghosterm.SSHCredentialFlowTest#'+method
            build(label)
            test(label, selection, False)
            test(label+'-control', 'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', True)
            path.write_bytes(originals[path])
            assert hashlib.sha256(path.read_bytes()).digest() == hashes[path]
            build(label+'-restored')
            test(label+'-restored', selection, True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256', flush=True)
    finally:
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes():
                path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest() != h for p, h in hashes.items()):
            raise RuntimeError('Credential source restoration failed')
    print(f'ANDROID CREDENTIAL MUTATIONS PASS: {len(cases)} groups', flush=True)


if __name__ == '__main__':
    raise SystemExit('Use ANDROID_SSH_CREDENTIAL_MUTATE=1 with scripts/test-android-ssh.sh')
