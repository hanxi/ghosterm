#!/usr/bin/env python3
"""Host identity product contract: compiled APK target red, independent config green, SHA restore."""
import hashlib

MARK = 'GT_ANDROID_HOST_IDENTITIES_MUTATION'

def execute(root, build, test):
    app = root/'android/app/src/main/java/com/hanxi/ghosterm'
    data = root/'android/data/src/main/java/com/hanxi/ghosterm/data'
    repo = data/'ConfigRepository.kt'
    refs = data/'IdentityReferences.kt'
    model = app/'HostsViewModel.kt'
    details = app/'IdentitiesViewModel.kt'
    auth = app/'SSHConnectionDialog.kt'
    domain = root/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/Hosts.kt'
    native = root/'android/bridge/src/main/cpp/config_jni.c'
    screen = app/'MainActivity.kt'
    originals = {p:p.read_bytes() for p in (repo,refs,model,details,auth,domain,native,screen)}
    hashes = {p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()): raise RuntimeError('Residual host identity mutation')
    cases = [
        ('quote',native,"if (value[n] == '\"' || value[n] == '\\\\')",'if (false)',
         'orderedIdentityEditsKeepInheritedIncludeAndOtherBytes'),
        ('untouched-lines',repo,'if (withoutOwnIdentityLines(fieldsEdited) != withoutOwnIdentityLines(candidate))',
         'if (false && withoutOwnIdentityLines(fieldsEdited) != withoutOwnIdentityLines(candidate))',
         'identityEditRejectsCorruptEditorBackupFailureAndReadOnlyOwnership'),
        ('requested-values',repo,'if (declared != edit.identityFiles)', 'if (false && declared != edit.identityFiles)',
         'identityEditRejectsCorruptEditorBackupFailureAndReadOnlyOwnership'),
        ('dirty',domain,'if (identityFiles != null) return true','if (false && identityFiles != null) return true',
         'editorDraftSurvivesRotationAndRestoreInheritancePreservesSource'),
        ('candidate',refs,'byId[id]?.let','available.firstOrNull()?.let',
         'referenceCodecAndCandidatesNeverSubstituteUnrelatedKeys'),
        ('manual-choice',auth,'attempt.target.identityFiles.isEmpty() || otherIdentity', 'true',
         'missingConfiguredIdentityRequiresExplicitOneTimeSelection'),
        ('references',refs,'if (sources.isNotEmpty()) add(IdentityUsage(host.alias, sources))',
         'if (false && sources.isNotEmpty()) add(IdentityUsage(host.alias, sources))',
         'referenceDetailsDistinguishConfigFailureFromZeroAndRefreshAfterRename'),
        ('reference-error',details,'usageError = if (loaded.second.isFailure)', 'usageError = if (false && loaded.second.isFailure)',
         'referenceDetailsDistinguishConfigFailureFromZeroAndRefreshAfterRename'),
        ('source-error',model,'identityError = "无法读取身份来源或身份列表，草稿已保留；可重试或查看 SSH 配置。"',
         'identityError = null','identitySourceFailureKeepsScalarDraftAndNeverShowsEmptySuccess'),
        ('active-session',screen,'hosts.identities(); identities.enter()',
         'hosts.identities(); sessions.sessions.map { it.id }.forEach(sessions::close); identities.enter()',
         'hostEditorAuthenticationRenameAndRemovalKeepActiveRemotePid'),
    ]
    # Reject stale/ambiguous anchors before modifying or building any source.
    for label,path,old,_,_ in cases:
        if originals[path].decode().count(old) != 1: raise RuntimeError('Ambiguous host identity mutation '+label)
    try:
        for label,path,old,new,method in cases:
            selection = 'com.hanxi.ghosterm.HostIdentityTest#'+method
            label = 'host-identities-'+label
            path.write_text(originals[path].decode().replace(old,new)+'\n// '+MARK+'\n')
            build(label); test(label,selection,False)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path])
            assert hashlib.sha256(path.read_bytes()).digest() == hashes[path]
            build(label+'-restored'); test(label+'-restored',selection,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes(): path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()): raise RuntimeError('Host identity source restoration failed')
    print(f'ANDROID HOST IDENTITY MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__': raise SystemExit('Use ANDROID_SSH_HOST_IDENTITIES_MUTATE=1 with scripts/test-android-ssh.sh')
