#!/usr/bin/env python3
"""TCP candidate scheduling against real SSH and isolated emulator packet loss."""
import hashlib
import os

MARK = 'GT_ANDROID_SSH_NETWORK_MUTATION'


def execute(root, build, test):
    native = root / 'android/bridge/src/main/cpp/ssh_session.c'
    bridge = root / 'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/RemoteSession.kt'
    session = root / 'android/bridge/src/main/cpp/session_jni.c'
    model = root / 'android/app/src/main/java/com/hanxi/ghosterm/SessionsViewModel.kt'
    originals = {p: p.read_bytes() for p in (native, bridge, session, model)}
    hashes = {p: hashlib.sha256(data).digest() for p, data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual network mutation')
    invalid = 'invalidCandidateSetsDoNotCreateOwners'
    cases = [
        ('advance', native, 'candidate+1<s->address_count', '(candidate+1<s->address_count && false)',
         'refusedIpv4FallsBackAndPreservesHostIdentity'),
        ('tcp-grace', native, '#define GAJ_SSH_TCP_CANDIDATE_MS 2000', '#define GAJ_SSH_TCP_CANDIDATE_MS 60000',
         'tcpBlackholeAdvancesAndDuplicateAddressesAreSkipped'),
        ('duplicates', native, 'if (!duplicate) s->addresses', 'if (!duplicate || duplicate) s->addresses',
         'tcpBlackholeAdvancesAndDuplicateAddressesAreSkipped'),
        ('total-budget', native, 'options.timeout_ms=(uint32_t)(deadline-now);', 'options.timeout_ms=s->options.timeout_ms;',
         'candidateSwitchDoesNotResetTotalDeadline'),
        ('negotiation', native, 'const bool was_tcp=info.phase==GT_SSH_AUTH_PHASE_TCP;', 'const bool was_tcp=true;',
         'negotiationIoFailureDoesNotTryAnotherAddress'),
        ('prompt-grace', native, 'info.phase==GT_SSH_AUTH_PHASE_TCP && now-candidate_started',
         'info.phase!=GT_SSH_AUTH_PHASE_FAILED && now-candidate_started',
         'hostPromptRetainsItsOwnerPastTcpGraceAndCancellationReleasesIt'),
        ('address-copy', bridge, 'private val binaryAddress = address.copyOf()', 'private val binaryAddress = address',
         'candidateSnapshotDoesNotFollowCallerMutation'),
        ('list-copy', bridge, 'private val candidates = addresses.toList()', 'private val candidates = addresses',
         'candidateSnapshotDoesNotFollowCallerMutation'),
        ('address-limit', native, 'count>GAJ_SSH_MAX_ADDRESSES', 'count>128', invalid),
        ('scope-negative', native, 'scope<0 ||', 'scope< -1 ||', invalid),
        ('scope-ipv4', native, '(n==4 && scope!=0)', 'false', invalid),
        ('ipv6', native, '.family=n==4?4:6', '.family=4',
         'ipv4FailureReachesRealIpv6AndIpv6FailureReachesIpv4'),
        ('model-candidates', model, 'val candidates = addresses.map', 'val candidates = addresses.take(1).map',
         'productPassesAllResolvedAddressesToNativeOwner'),
        ('cancel', session, 'atomic_store(&s->stop, true); }', 'atomic_store(&s->stop, false); }',
         'cancelledTcpDoesNotStartAnotherCandidateAndOwnerReleases'),
    ]
    selected = os.environ.get('ANDROID_SSH_NETWORK_MUTATIONS', '').split(',')
    if selected != ['']:
        unknown = set(selected) - {case[0] for case in cases}
        if unknown:
            raise RuntimeError('Unknown network mutation selection: ' + ','.join(sorted(unknown)))
        cases = [case for case in cases if case[0] in selected]
    try:
        for label, path, old, new, method in cases:
            source = originals[path].decode()
            if source.count(old) != 1:
                raise RuntimeError('Ambiguous network mutation ' + label)
            path.write_text(source.replace(old, new) + '\n// ' + MARK + '\n')
            label = 'ssh-network-' + label
            selection = 'com.hanxi.ghosterm.SSHNetworkTest#' + method
            build(label)
            test(label, selection, False)
            test(label + '-control', 'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', True)
            path.write_bytes(originals[path])
            assert hashlib.sha256(path.read_bytes()).digest() == hashes[path]
            build(label + '-restored'); test(label + '-restored', selection, True)
            print('PASS ' + label + ': compiled target red / same APK config green / restored green / SHA256', flush=True)
    finally:
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes(): path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest() != h for p, h in hashes.items()):
            raise RuntimeError('Network source restoration failed')
    print(f'ANDROID SSH NETWORK MUTATIONS PASS: {len(cases)} groups', flush=True)


if __name__ == '__main__':
    raise SystemExit('Use ANDROID_SSH_NETWORK_MUTATE=1 with scripts/test-android-ssh.sh on a rooted arm64 emulator')
