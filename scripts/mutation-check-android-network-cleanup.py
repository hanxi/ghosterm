#!/usr/bin/env python3
"""Real emulator loopback-rule cleanup, isolated in-memory mutation and untouched control rule."""
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
def rule(label): return ['OUTPUT','-p','tcp','-d','127.0.0.2','--dport','65534','-m','comment','--comment',label+'-probe','-j','DROP']
def rows(): return m.run([*adb,'shell','su','0','iptables','-S','OUTPUT'])
try:
    for label in (prefix,sentinel):m.run([*adb,'shell','su','0','iptables','-I',*rule(label)])
    assert prefix in rows() and sentinel in rows()
    ns={'run':m.run,'shlex':shlex}
    source=inspect.getsource(m.cleanup_network_rules)
    old="run([*adb,'shell','su','0','iptables','-D',*fields[1:]])"
    assert source.count(old)==1
    exec(source.replace(old,'pass'),ns)
    try:ns['cleanup_network_rules'](adb,prefix)
    except RuntimeError as error:assert str(error)=='Network fixture rules survived cleanup'
    else:raise AssertionError('cleanup mutation did not fail')
    assert sentinel in rows()
    m.cleanup_network_rules(adb,prefix)
    assert prefix not in rows() and sentinel in rows()
    print('PASS cleanup: isolated removal mutation rejected, independent sentinel preserved, production removal restored')
finally:
    m.cleanup_network_rules(adb,prefix);m.cleanup_network_rules(adb,sentinel)
assert prefix not in rows() and sentinel not in rows()
assert hashlib.sha256((root/"scripts/test-android-ssh.py").read_bytes()).digest()==original
