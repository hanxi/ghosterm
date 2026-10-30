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
    if MARK.encode() in original or b'GT_MUTATION_MARK' in original:
        raise RuntimeError('residual known-host mutation marker')
    logs = ROOT / 'tmp/known-hosts-mutations'; logs.mkdir(parents=True, exist_ok=True)
    cases = [
        ('K1-ecdsa', 'if (!gt__kh_split_line(line, &entry) || !entry.recognized) continue;',
         'if (!gt__kh_split_line(line, &entry) || !entry.recognized || strncmp(entry.key_type,"ssh-",4)) continue;', None),
        ('K2-all-lines', 'if (!cb(ud, entry.hosts, entry.key_type, entry.key_base64, entry.marker)) break;',
         'cb(ud, entry.hosts, entry.key_type, entry.key_base64, entry.marker); break;', None),
        ('K3-bare-host', ' || kh_pattern(pattern,n,q->host)', '', None),
        ('K4-marker', 'entry.key_base64, entry.marker)', 'entry.key_base64, "")', 'revoked-only'),
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
        ('K16-write-result', 'gt__set_error("known_hosts 写入失败: %s", strerror(written < 0 ? write_error : errno));\n    return GT_ERR_IO;',
         'gt__set_error("known_hosts 写入失败: %s", strerror(written < 0 ? write_error : errno));\n    return GT_OK;', 'save-write-failure async'),
        ('K17-flush-result', 'if (written < 0 || closed != 0)', '(void)closed; if (written < 0)', 'save-flush-failure'),
        ('K18-sync-save', 'if (saved != GT_OK) { free(b64); free(fingerprint); return saved; }',
         'if (false && saved != GT_OK) { free(b64); free(fingerprint); return saved; }', 'save-open-failure sync'),
        ('K19-async-save', 'if(rc!=GT_OK){connector_fail(c,rc);return rc;}',
         'if(false && rc!=GT_OK){connector_fail(c,rc);return rc;}', 'save-open-failure async'),
        ('K20-open-result', 'gt__set_error("known_hosts 写入失败: %s", strerror(errno));\n    return GT_ERR_IO;',
         'gt__set_error("known_hosts 写入失败: %s", strerror(errno));\n    return GT_OK;', 'save-open-failure'),
    ]
    with tempfile.TemporaryDirectory(prefix='kh-mutation-', dir=ROOT / 'tmp') as directory:
        work = Path(directory)
        shutil.copytree(ROOT / 'core', work / 'core', ignore=shutil.ignore_patterns('build', 'build-*'))
        copy = work / 'core/src/ssh.c'; build = work / 'core/build'
        helper.require(helper.run(['cmake', '-S', work / 'core', '-B', build, '-DCMAKE_BUILD_TYPE=Debug'], logs / 'configure.log'))
        def compile_core(label):
            helper.require(helper.run(['cmake', '--build', build, '--clean-first', '--target', 'test_known_hosts', 'test_config'], logs / (label+'-build.log')))
        def target(label, reason=None):
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
                text=original.decode()
                if text.count(old)!=1: raise RuntimeError('mutation anchor drift: '+label)
                copy.write_text(text.replace(old,new+' /* '+MARK+' */'))
                compile_core(label); target(label,reason); control(label)
                copy.write_bytes(original)
                if hashlib.sha256(copy.read_bytes()).digest()!=digest: raise RuntimeError('copy restore SHA mismatch')
                compile_core(label+'-restored'); target(label+'-restored')
                print(label+': compiled target red / same-mutant config green / restored green / SHA256',flush=True)
        finally:
            copy.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest()!=digest: raise RuntimeError('production source changed')
    print(f'KNOWN HOSTS MUTATIONS PASS: {len(cases)} groups',flush=True)


if __name__=='__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(signum)
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, interrupted)
    os.environ.setdefault('TMPDIR',str(ROOT/'tmp'))
    main()
