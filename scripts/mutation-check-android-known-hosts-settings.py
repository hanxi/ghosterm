#!/usr/bin/env python3
"""Known-host management product assertions; real SSH and same-APK controls."""
import hashlib
MARK = 'GT_ANDROID_KNOWN_HOSTS_SETTINGS_MUTATION'


def execute(root, build, test):
    base = root / 'android'
    model = base / 'app/src/main/java/com/hanxi/ghosterm/KnownHostsViewModel.kt'
    screen = base / 'app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    repository = base / 'data/src/main/java/com/hanxi/ghosterm/data/KnownHostsRepository.kt'
    public = base / 'data/src/main/java/com/hanxi/ghosterm/data/IdentityPublicInfo.kt'
    originals = {p: p.read_bytes() for p in (model, screen, repository, public)}
    hashes = {p: hashlib.sha256(data).digest() for p, data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual known-host settings mutation')
    detail = 'duplicateRemovalUsesExactRecordAndConfirmationSurvivesRecreation'
    info = 'publicFingerprintUsesWireBytesAndRejectsWrongAlgorithms'
    files = 'repositoryRejectsLinksSpecialFilesAndInvalidBytes'
    outcome = 'committedConflictAndUnknownResultsRemainDistinctAndReload'
    cases = [
        ('entry', screen, 'hosts.knownHosts(); knownHosts.enter()', 'hosts.knownHosts(); Unit', detail),
        ('confirmation', model, 'if (!busy && selected?.removable == true) confirmingRemoval = true',
         'if (!busy && selected?.removable == true) { confirmingRemoval = true; remove() }', detail),
        ('record-index', repository, 'snapshot.expectedBytes(), entry.index)', 'snapshot.expectedBytes(), 0)', detail),
        ('snapshot-copy', repository, 'private val original = bytes.copyOf()', 'private val original = bytes', files),
        ('fingerprint', public, 'MessageDigest.getInstance("SHA-256")', 'MessageDigest.getInstance("SHA-512")', info),
        ('algorithm', public, 'expectedType == null || type == expectedType', 'expectedType == null || type.isNotEmpty()', info),
        ('readonly-rules', repository, 'record.recognized && record.marker.isEmpty() && !record.hashed', 'record.recognized',
         'ruleKindsAndMalformedRecordsStayVisibleAndReadOnly'),
        ('removal-guard', repository, 'entry != null && entry.removable', 'entry != null',
         'readOnlyRecordsCannotBeRemovedThroughRepository'),
        ('regular-file', repository, 'OsConstants.S_ISREG(before.st_mode)', 'true', files),
        ('parent-link', repository, 'check(OsConstants.S_ISDIR(parent.st_mode))', 'check(true)', files),
        ('escaped-host', repository, 'Character.isISOControl(c) || Character.getType(c) == Character.FORMAT.toInt()', 'false', info),
        ('read-error', model,
         'if (present && generation == token) error = "无法读取主机验证记录：${failure.message ?: "请重试"}"',
         'if (present && generation == token) snapshot = KnownHostsSnapshot(byteArrayOf(), "hidden-error")',
         'readFailureIsNotEmptySuccessAndReloadRecovers'),
        ('stale-read', model, 'generation++; readJob?.cancel(); readJob = null;', 'readJob = null;',
         'lateReadAfterLeavingCannotReplaceNewSnapshot'),
        ('committed-result', model, 'result.committed ->', 'false ->', outcome),
        ('conflict-result', model, 'result.conflict ->', 'false ->', outcome),
        ('unknown-result', model, '移除结果无法确认，请重新载入并核对记录。', '本次移除未提交：未知错误', outcome),
        ('write-navigation', model, 'present = false; cancelRead();',
         'present = false; viewModelScope.coroutineContext.cancelChildren(); cancelRead();',
         'confirmedWriteSurvivesLeavingAndReentryWithoutDuplicateSubmission'),
    ]
    try:
        for label, path, old, new, method in cases:
            source = originals[path].decode()
            if source.count(old) != 1:
                raise RuntimeError('Ambiguous known-host settings mutation ' + label)
            path.write_text(source.replace(old, new) + '\n// ' + MARK + '\n')
            label = 'known-host-settings-' + label
            selection = 'com.hanxi.ghosterm.KnownHostsSettingsTest#' + method
            build(label)
            test(label, selection, False)
            test(label + '-control', 'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', True)
            path.write_bytes(originals[path])
            assert hashlib.sha256(path.read_bytes()).digest() == hashes[path]
            build(label + '-restored')
            test(label + '-restored', selection, True)
            print('PASS ' + label + ': compiled target red / same APK config green / restored green / SHA256', flush=True)
    finally:
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes():
                path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest() != h for p, h in hashes.items()):
            raise RuntimeError('Known-host settings source restoration failed')
    print(f'ANDROID KNOWN HOST SETTINGS MUTATIONS PASS: {len(cases)} groups', flush=True)


if __name__ == '__main__':
    raise SystemExit('Use ANDROID_SSH_KNOWN_HOSTS_SETTINGS_MUTATE=1 with scripts/test-android-ssh.sh')
