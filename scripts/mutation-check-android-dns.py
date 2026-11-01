#!/usr/bin/env python3
"""DNS resolution, cancellation and diagnostics with actual system DNS and SSH controls."""
import hashlib
import os

MARK = 'GT_ANDROID_DNS_MUTATION'


def execute(root, build, test):
    resolver=root/'android/app/src/main/java/com/hanxi/ghosterm/SSHResolver.kt'
    wire=root/'android/app/src/androidTest/java/com/hanxi/ghosterm/SSHDnsWireTest.kt'
    originals={p:p.read_bytes() for p in (resolver,wire)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual DNS mutation')
    cases=[
        ('numeric-address', 'SSHDnsResolverTest#numericIpv4AndScopedIpv6NeverQueryDns', 'InetAddresses.parseNumericAddress(host)', 'InetAddresses.parseNumericAddress("127.0.0.2")', 1),
        ('numeric-scope', 'SSHDnsResolverTest#numericIpv4AndScopedIpv6NeverQueryDns', 'InetAddresses.parseNumericAddress(host)', "InetAddresses.parseNumericAddress(host.substringBefore('%'))", 1),
        ('idn', 'SSHDnsResolverTest#internationalDomainIsAsciiForDnsWithoutChangingOriginalInput', 'dnsName(host)', 'host', 1),
        ('invalid-name', 'SSHDnsResolverTest#invalidNamesDoNotStartPlatformQuery', 'host.none { it.isWhitespace() || it.isISOControl() }', 'true', 1),
        ('nxdomain', 'SSHDnsResolverTest#nxdomainEmptyAndServerFailureAreDistinctAndNeverReturnAnswers', 'rcode==3 ->', 'rcode==999 ->', 1),
        ('rcode-failure', 'SSHDnsResolverTest#nxdomainEmptyAndServerFailureAreDistinctAndNeverReturnAnswers', 'rcode!=0 ->', 'false ->', 1),
        ('empty-answer', 'SSHDnsResolverTest#nxdomainEmptyAndServerFailureAreDistinctAndNeverReturnAnswers', 'answer.isEmpty() ->', 'false ->', 1),
        ('address-limit', 'SSHDnsResolverTest#addressLimitRejectsInsteadOfSilentlyTruncating', 'answer.size>32', 'answer.size>128', 1),
        ('answer-snapshot', 'SSHDnsResolverTest#returnedAddressesAreAnOrderedSnapshotAndKeepIpv6Scope', 'waiting.resume(answer.toList())', 'waiting.resume(answer)', 1),
        ('cancel-signal', 'SSHDnsResolverTest#cancellationReachesAndroidAndLateCallbacksCannotCompleteTheRequest', 'waiting.invokeOnCancellation { cancellation.cancel() }', 'waiting.invokeOnCancellation { Unit }', 1),
        ('late-callback', 'SSHDnsResolverTest#cancellationReachesAndroidAndLateCallbacksCannotCompleteTheRequest', 'if(!waiting.isActive)return', 'if(false)return', 2),
        ('parse-error', 'SSHDnsResolverTest#parseNetworkAndTimeoutErrorsRetainTheirCausesAndCategories', 'error.code==DnsResolver.ERROR_PARSE -> SSHResolveFailure.RESPONSE_INVALID', 'false -> SSHResolveFailure.RESPONSE_INVALID', 1),
        ('network-error', 'SSHDnsResolverTest#parseNetworkAndTimeoutErrorsRetainTheirCausesAndCategories', 'val unavailable=error.code==DnsResolver.ERROR_SYSTEM && errno in setOf(ANDROID_ENONET,OsConstants.ENETDOWN,OsConstants.ENETUNREACH)', 'val unavailable=false', 1),
        ('timeout-text', 'SSHDnsResolverTest#parseNetworkAndTimeoutErrorsRetainTheirCausesAndCategories', 'error.code==DnsResolver.ERROR_SYSTEM && errno==OsConstants.ETIMEDOUT ->', 'false ->', 1),
        ('original-cause', 'SSHDnsResolverTest#parseNetworkAndTimeoutErrorsRetainTheirCausesAndCategories', 'cause=error.original', 'cause=null', 1),
        ('query-cancel', 'SSHDnsResolverTest#synchronousQueryFailureIsExplicitAndCancellationIsNotNetworkFailure', 'waiting.cancel(cancel)', 'waiting.resumeWithException(SSHResolveException(SSHResolveFailure.DNS_FAILURE,"cancelled"))', 1),
        ('product-text', 'SSHDnsResolverTest#dnsFailureIsVisibleInProductAndCreatesNoTerminal', 'DNS 未找到此域名，请检查主机地址', '请检查网络', 1),
        ('platform-answer', 'SSHDnsWireTest#realDnsAnswersReachSshAndPreserveUnicodeHostIdentity', 'callback.onAnswer(answer,rcode)', 'callback.onAnswer(emptyList(),rcode)', 1),
        ('platform-rcode', 'SSHDnsWireTest#actualNxdomainEmptyAndServfailRemainDistinct', 'callback.onAnswer(answer,rcode)', 'callback.onAnswer(answer,0)', 1),
    ]
    cases += [('bionic-enonet', 'SSHDnsResolverTest#parseNetworkAndTimeoutErrorsRetainTheirCausesAndCategories', 'private const val ANDROID_ENONET = 64', 'private const val ANDROID_ENONET = 65', 1)]
    cases += [('late-wire-answer', 'SSHDnsWireTest#actualDnsCancellationAndLateAnswerCannotCreateAnOwner', 'reply(it,Mode.POSITIVE,true)', 'Unit', 1, 'wire')]
    cases += [
        ('nontransitional', 'SSHDnsResolverTest#unicodeDeviationCharactersDoNotResolveToADifferentAsciiDomain', 'IDNA.NONTRANSITIONAL_TO_ASCII', '0', 1),
        ('idna-validation', 'SSHDnsResolverTest#invalidNamesDoNotStartPlatformQuery', 'require(!info.hasErrors())', 'require(true)', 1),
    ]
    cases += [
        ('probe-fallback', 'SSHDnsWireTest#domainResolutionDoesNotDependOnPublicProbeRoutes', 'val retry=error.code==DnsResolver.ERROR_SYSTEM && errno in setOf(ANDROID_ENONET,OsConstants.ENETDOWN,OsConstants.ENETUNREACH)', 'val retry=false', 1),
        ('probe-error-scope', 'SSHDnsCompatibilityTest#ordinaryFailuresDoNotStartCompatibilityQueries', 'val retry=error.code==DnsResolver.ERROR_SYSTEM && errno in setOf(ANDROID_ENONET,OsConstants.ENETDOWN,OsConstants.ENETUNREACH)', 'val retry=true', 1),
        ('family-types', 'SSHDnsCompatibilityTest#preflightFailuresQueryBothFamiliesAndPreserveIpv6Scope', 'listOf(SSHDnsRecordType.A,SSHDnsRecordType.AAAA)', 'listOf(SSHDnsRecordType.A,SSHDnsRecordType.A)', 1),
        ('family-signals', 'SSHDnsCompatibilityTest#cancellationReachesThreeIndependentSignalsAndRejectsLateCallbacks', 'records.query(host,type,signals[index+1],sink)', 'records.query(host,type,signals[1],sink)', 1),
        ('family-cancel', 'SSHDnsCompatibilityTest#cancellationReachesThreeIndependentSignalsAndRejectsLateCallbacks', 'cancellation.setOnCancelListener { signals.forEach { it.cancel() } }', 'cancellation.setOnCancelListener { signals[0].cancel() }', 1),
        ('family-limit', 'SSHDnsCompatibilityTest#combinedLimitRejectsRatherThanTruncatingAnAddressFamily', 'return Reply(addresses)', 'return Reply(addresses.take(32))', 1),
        ('family-rcode', 'SSHDnsCompatibilityTest#serverErrorsNeverContributeAddressesOrInventNxdomain', 'it.error==null && it.rcode==0', 'it.error==null', 1),
        ('family-snapshot', 'SSHDnsCompatibilityTest#stalePrimaryAndDuplicateFamilyCallbacksCannotReplaceTheSnapshot', 'Reply(answer.toList(),rcode)', 'Reply(answer,rcode)', 2),
        ('family-duplicates', 'SSHDnsCompatibilityTest#stalePrimaryAndDuplicateFamilyCallbacksCannotReplaceTheSnapshot', '|| replies[index]!=null', '|| false', 1),
        ('family-late-primary', 'SSHDnsCompatibilityTest#stalePrimaryAndDuplicateFamilyCallbacksCannotReplaceTheSnapshot', 'if(stage!=0 || cancellation.isCanceled)false', 'if(cancellation.isCanceled)false', 1),
        ('family-startup-cleanup', 'SSHDnsCompatibilityTest#synchronousFailuresReleaseSignalsAndDoNotChangeCancellationSemantics', 'signals[index+1].cancel();sink.onError(', 'sink.onError(', 1),
        ('primary-startup-cleanup', 'SSHDnsCompatibilityTest#synchronousFailuresReleaseSignalsAndDoNotChangeCancellationSemantics', 'synchronized(lock) { stage=2 }\n            signals.forEach { it.cancel() }\n            throw error', 'synchronized(lock) { stage=2 }\n            throw error', 1),
        ('network-diagnostic', 'SSHDnsCompatibilityTest#systemDnsFailureDoesNotClaimAllConnectivityIsUnavailable', '系统 DNS 无法访问当前解析网络，请检查 DNS 设置或稍后重试', '当前网络不可用，无法解析主机地址', 1),
    ]
    selected=os.environ.get('ANDROID_SSH_DNS_MUTATIONS','').split(',')
    if selected!=['']:
        unknown=set(selected)-{case[0] for case in cases}
        if unknown:raise RuntimeError('Unknown DNS cases: '+','.join(sorted(unknown)))
        cases=[case for case in cases if case[0] in selected]
    for name,target,old,new,count,*where in cases:
        path=wire if where else resolver
        if originals[path].decode().count(old)!=count:raise RuntimeError('Ambiguous DNS mutation '+name)
    def one(label,selection,green):
        import re
        output=test(label,selection,green)
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output:raise RuntimeError(output)
        if green:
            valid='OK (1 test)' in output and 'FAILURES' not in output
        else:
            valid=bool(re.search(r'Tests run: 1,\s+Failures: 1',output)) and any(kind in output for kind in ('java.lang.AssertionError','org.junit.ComparisonFailure'))
        if not valid:raise RuntimeError(output)
    try:
        for name,target,old,new,count,*where in cases:
            path=wire if where else resolver;source=originals[path].decode()
            path.write_text(source.replace(old,new)+'\n// '+MARK+'\n')
            label='dns-'+name;selection='com.hanxi.ghosterm.'+target
            build(label);one(label,selection,False)
            one(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');one(label+'-restored',selection,True)
            print('PASS '+label+': compiled single assertion red / same APK Core green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('DNS source restoration failed')
    print(f'ANDROID DNS MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    raise SystemExit('Use ANDROID_SSH_DNS_MUTATE=1 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.SSHDnsResolverTest,com.hanxi.ghosterm.SSHDnsWireTest with scripts/test-android-ssh.sh')
