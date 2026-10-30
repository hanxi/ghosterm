#!/usr/bin/env python3
"""Settings identity lifecycle against real DocumentsUI, Keystore and SSH."""
import hashlib
MARK='GT_ANDROID_IDENTITIES_MUTATION'

def execute(root,build,test):
    base=root/'android'
    model=base/'app/src/main/java/com/hanxi/ghosterm/IdentitiesViewModel.kt'
    screen=base/'app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    vault=base/'data/src/main/java/com/hanxi/ghosterm/data/IdentityVault.kt'
    public=base/'data/src/main/java/com/hanxi/ghosterm/data/IdentityPublicInfo.kt'
    originals={p:p.read_bytes() for p in (model,screen,vault,public)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual identities mutation')
    details='settingsFingerprintAndConfirmedRemovalSurviveRecreation'
    cases=[
        ('fingerprint',public,'MessageDigest.getInstance("SHA-256")','MessageDigest.getInstance("SHA-512")',
         'encryptedPublicInfoMatchesOracleAndMalformedDataIsExplicit'),
        ('rsa-info',public,'"ssh-rsa" -> repeat(2)','"unsupported-rsa" -> repeat(2)',
         'rsaAndAllNistCurvesMatchIndependentOpenSshFingerprints'),
        ('ecdsa-info',public,'"nistp256" -> 65','"nistp256" -> 64',
         'rsaAndAllNistCurvesMatchIndependentOpenSshFingerprints'),
        ('stale-removal',vault,'check(record.revision != null && revision(encoded(record.id)) == record.revision) { "身份已变化，请重新载入后再移除" }',
         'check(record.revision != null) { "身份已变化，请重新载入后再移除" }','staleRemovalPreservesChangedEnvelopeAndKeystoreKey'),
        ('remove-access',vault,'keystore().deleteEntry(keyAlias(record.id))','Unit',details),
        ('corrupt-entry',vault,'IdentityRecord(id,"无法读取的身份",null,stamp,"身份资料不可读取，请移除后重新导入。")',
         'throw IllegalStateException("身份资料不可读取")','corruptEntryIsVisibleAndRemovableWithoutHidingHealthyKey'),
        ('entry',screen,'hosts.identities(); identities.enter()','hosts.identities(); Unit',details),
        ('late-result',model,'if (uri == null || token == null || !current(token)) return','if (uri == null) return',
         'pickerCancelAndLateResultAfterLeavingDoNotImport'),
        ('cancel-read',model,'generation++; job?.cancel(); job = null; busy = false','generation++; job = null; busy = false',
         'leavingDuringPipeReadClosesProviderWithoutSaving'),
        ('confirmation',model,'if (!busy && selected?.revision != null) confirmingRemoval = true',
         'if (!busy && selected?.revision != null) { confirmingRemoval = true; remove() }',details),
    ]
    try:
        for label,path,old,new,method in cases:
            source=originals[path].decode()
            if source.count(old)!=1:raise RuntimeError('Ambiguous identities mutation '+label)
            path.write_text(source.replace(old,new)+'\n// '+MARK+'\n')
            label='identities-'+label;selection='com.hanxi.ghosterm.IdentitySettingsTest#'+method
            build(label);test(label,selection,False)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored',selection,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Identities source restoration failed')
    print(f'ANDROID IDENTITIES MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_SSH_IDENTITIES_MUTATE=1 with scripts/test-android-ssh.sh')
