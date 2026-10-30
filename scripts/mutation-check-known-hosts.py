#!/usr/bin/env python3
"""Known-host trust: isolated C mutations, real sshd and independent config controls."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import signal
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_KNOWN_HOSTS_MUTATION'
spec = importlib.util.spec_from_file_location('helper', ROOT / 'scripts/mutation-check-config-rename.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    source = ROOT / 'core/src/ssh.c'
    original = source.read_bytes()
    digest = hashlib.sha256(original).digest()
    store = ROOT / 'core/src/known_hosts_store.c'
    store_original = store.read_bytes()
    if any(MARK.encode() in data or b'GT_MUTATION_MARK' in data for data in (original, store_original)):
        raise RuntimeError('residual known-host mutation marker')
    logs = ROOT / 'tmp/known-hosts-mutations'; logs.mkdir(parents=True, exist_ok=True)
    cases = [
        ('K1-ecdsa', 'if (!gt__kh_split_line(line, &entry) || !entry.recognized) continue;',
         'if (!gt__kh_split_line(line, &entry) || !entry.recognized || strncmp(entry.key_type,"ssh-",4)) continue;', None),
        ('K2-all-lines', 'if (!cb(ud, entry.hosts, entry.key_type, entry.key_base64, entry.marker)) break;',
         'cb(ud, entry.hosts, entry.key_type, entry.key_base64, entry.marker); break;', None),
        ('K3-bare-host', ' || kh_pattern(pattern,n,q->host)', '', None),
        ('K4-marker', 'cb(ud, entry.hosts, entry.key_type, entry.key_base64, entry.marker)', 'cb(ud, entry.hosts, entry.key_type, entry.key_base64, "")', 'revoked-only'),
        ('K5-revoked', 'if (q.revoked) return KH_REVOKED;', 'if (false && q.revoked) return KH_REVOKED;', 'revoked-only'),
        ('K6-order', 'return true; /* A later revocation always overrides ordinary trust. */',
         'return !q->matched;', 'revoked-after-trusted'),
        ('K7-unknown', 'if (*marker) { q->unsupported=true; return true; }',
         'if (*marker) { return true; }', 'unknown-marker'),
        ('K8-authority', 'q->authority=true;', 'q->matched=same;', 'authority-is-not-hostkey'),
        ('K9-wildcard', "else if (p<n && pattern[p]=='*')", "else if (false && p<n && pattern[p]=='*')", 'revoked-wildcard'),
        ('K10-negation', 'if (match && negated) return false;', 'if (false && match && negated) return false;', 'revoked-negated-host'),
        ('K11-other-key', 'if (same) q->revoked=true;', 'q->revoked=true;', 'revoked-other-key'),
        ('K12-hash', "if (hosts[0]=='|') { q->unsupported=true; return true; }", "if (hosts[0]=='|') { return true; }", 'hashed-revocation-not-ignored'),
        ('K13-read', 'return failed ? GT_ERR_IO : GT_OK;', '(void)failed; return GT_OK;', 'trust-read-error'),
        ('K14-sync', 'if (kh == KH_REVOKED || kh == KH_UNSUPPORTED || kh == KH_IO)',
         'if (kh == KH_UNSUPPORTED || kh == KH_IO)', 'revoked-only sync'),
        ('K15-async', 'if(result==KH_REVOKED){', 'if(false && result==KH_REVOKED){', 'revoked-only async'),
        ('K16-write-result', 'free(want);\n  return status;',
         'free(want);\n  (void)status; return GT_OK;', 'save-write-failure async'),
        # Buffered fprintf/fclose was replaced by atomic file transactions. This
        # now injects a real close error, not an RLIMIT write error called flush.
        ('K17-flush-result', 'if (close(fd) && !failure) failure = errno;\n  if (failure) { unlinkat',
         '(void)close(fd);\n  if (failure) { unlinkat', 'fault backup close'),
        ('K18-sync-save', 'if (saved != GT_OK) { free(b64); free(fingerprint); return saved; }',
         'if (false && saved != GT_OK) { free(b64); free(fingerprint); return saved; }', 'save-open-failure sync'),
        ('K19-async-save', 'if(rc!=GT_OK){connector_fail(c,rc);return rc;}',
         'if(false && rc!=GT_OK){connector_fail(c,rc);return rc;}', 'save-open-failure async'),
        ('K20-open-result', 'if (fd < 0) { name[0] = 0; return -1; }',
         'if (fd < 0) { name[0] = 0; return 0; }', 'fault backup open'),
        ('K21-recheck', 'c->observed = kh_decision(c->lookup);',
         'c->observed = KH_UNKNOWN;', 'consent-revoked'),
        ('K22-deduplicate', 'if (c->observed == KH_MATCH) return GT_OK;',
         'if (c->observed == KH_MATCH) c->observed = KH_UNKNOWN;', 'consent-already-saved'),
        ('K23-refreshed-revoked', 'if (refreshed == KH_REVOKED) c->info.host = GT_SSH_HOST_REVOKED;',
         'if (refreshed == KH_REVOKED) c->info.host = GT_SSH_HOST_UNKNOWN;', 'consent-revoked async state'),
        ('K24-refreshed-mismatch', 'else if (refreshed == KH_MISMATCH) c->info.host = GT_SSH_HOST_MISMATCH;',
         'else if (refreshed == KH_MISMATCH) c->info.host = GT_SSH_HOST_UNKNOWN;', 'consent-mismatch async state'),
        ('K25-record-limit', 'if (records >= 4096)', 'if (records > 4096)', 'consent-record-limit'),
    ]
    with tempfile.TemporaryDirectory(prefix='kh-mutation-', dir=ROOT / 'tmp') as directory:
        work = Path(directory)
        shutil.copytree(ROOT / 'core', work / 'core', ignore=shutil.ignore_patterns('build', 'build-*'))
        copy = work / 'core/src/ssh.c'; build = work / 'core/build'
        helper.require(helper.run(['cmake', '-S', work / 'core', '-B', build, '-DCMAKE_BUILD_TYPE=Debug'], logs / 'configure.log'))
        def compile_core(label):
            helper.require(helper.run(['cmake', '--build', build, '--clean-first', '--target', 'test_known_hosts', 'test_known_hosts_store', 'test_config'], logs / (label+'-build.log')))
        (work / 'tmp').mkdir(exist_ok=True)
        def target(label, reason=None):
            if label.startswith(('K17-', 'K20-')):
                result = helper.run([build/'test_known_hosts_store',work/'tmp'], logs/(label+'-store.log'))
                out = result.stdout + result.stderr
                if reason is None: helper.require(result)
                elif result.returncode != 1 or 'FAIL store: '+reason not in out: raise RuntimeError('wrong store failure: '+out)
                return
            failed = False
            for algorithm, harness in [('ed25519','ssh_e2e.sh'), ('ecdsa','ecdsa_e2e.sh')]:
                result = helper.run(['bash', ROOT/'core/tests/scripts'/harness, build/'test_known_hosts'], logs/(label+'-'+algorithm+'.log'))
                out = result.stdout + result.stderr
                if 'SKIP' in out or 'test_known_hosts:' not in out:
                    raise RuntimeError('required real sshd unavailable: '+out)
                if reason is not None:
                    if result.returncode == 1 and ('FAIL: '+reason) in out: failed=True
                    elif result.returncode != 0: raise RuntimeError('wrong failure: '+out)
                elif label.startswith('K') and not label.endswith('-restored'):
                    if result.returncode == 1 and 'FAIL:' in out: failed=True
                    elif result.returncode != 0: raise RuntimeError(out)
                else: helper.require(result)
            if label.startswith('K') and not label.endswith('-restored') and not failed:
                raise RuntimeError('target did not fail: '+label+'; logs: '+str(logs))
        def control(label):
            helper.require(helper.run([build/'test_config', ROOT/'core/tests/fixtures'], logs/(label+'-control.log')))
        try:
            compile_core('baseline'); target('baseline'); control('baseline')
            for label, old, new, reason in cases:
                changed = work/'core/src/known_hosts_store.c' if label.startswith(('K17-', 'K20-')) else copy
                before_bytes = store_original if changed != copy else original
                text=before_bytes.decode()
                if text.count(old)!=1: raise RuntimeError('mutation anchor drift: '+label)
                changed.write_text(text.replace(old,new) + '\n/* '+MARK+' */\n')
                compile_core(label); target(label,reason); control(label)
                changed.write_bytes(before_bytes)
                if hashlib.sha256(changed.read_bytes()).digest()!=hashlib.sha256(before_bytes).digest(): raise RuntimeError('copy restore SHA mismatch')
                compile_core(label+'-restored'); target(label+'-restored')
                print(label+': compiled target red / same-mutant config green / restored green / SHA256',flush=True)
        finally:
            copy.write_bytes(original)
            (work/'core/src/known_hosts_store.c').write_bytes(store_original)
            if store.read_bytes() != store_original or hashlib.sha256(source.read_bytes()).digest()!=digest: raise RuntimeError('production source changed')
    print(f'KNOWN HOSTS MUTATIONS PASS: {len(cases)} groups',flush=True)


if __name__=='__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(signum)
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, interrupted)
    os.environ.setdefault('TMPDIR',str(ROOT/'tmp'))
    main()
