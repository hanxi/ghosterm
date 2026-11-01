#!/usr/bin/env python3
"""Real scoped DNS NAT-rule cleanup and an independent untouched sentinel."""
import importlib.util,inspect,secrets,shlex,os,hashlib,sys
sys.dont_write_bytecode=True
from pathlib import Path
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('runner',root/'scripts/test-android-ssh.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
adb=['adb','-s',os.environ['ANDROID_SERIAL']]
assert m.run([*adb,'shell','getprop','ro.kernel.qemu']).strip()=='1', 'Requires emulator'
original=hashlib.sha256((root/'scripts/test-android-ssh.py').read_bytes()).digest()
prefix='ghosterm-network-'+secrets.token_hex(8)
sentinel='ghosterm-network-'+secrets.token_hex(8)
def rule(label): return ['OUTPUT','-p','udp','--dport','53','-m','string','--algo','bm','--string','gtdns-cleanup-'+label[-16:],'-m','comment','--comment',label+'-dns-probe','-j','DNAT','--to-destination','127.0.0.1:65534']
def rows(): return m.run([*adb,'shell','su','0','iptables','-t','nat','-S','OUTPUT'])
try:
    for label in (prefix,sentinel):m.run([*adb,'shell','su','0','iptables','-t','nat','-I',*rule(label)])
    assert prefix in rows() and sentinel in rows()
    ns={'run':m.run,'shlex':shlex}
    source=inspect.getsource(m.cleanup_dns_rules)
    old="run([*adb,'shell','su','0','iptables','-t','nat','-D',*fields[1:]])"
    assert source.count(old)==1
    exec(source.replace(old,'pass'),ns)
    try:ns['cleanup_dns_rules'](adb,prefix)
    except RuntimeError as error:assert str(error)=='DNS fixture rules survived cleanup'
    else:raise AssertionError('cleanup mutation did not fail')
    assert sentinel in rows()
    m.cleanup_dns_rules(adb,prefix)
    assert prefix not in rows() and sentinel in rows()
    print('PASS DNS cleanup: isolated removal mutation rejected, independent sentinel preserved, production removal restored')
finally:
    m.cleanup_dns_rules(adb,prefix);m.cleanup_dns_rules(adb,sentinel)
assert prefix not in rows() and sentinel not in rows()
assert hashlib.sha256((root/"scripts/test-android-ssh.py").read_bytes()).digest()==original
