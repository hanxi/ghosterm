#!/usr/bin/env python3
"""Dedicated API 30+ emulator: permission-page process death and browser-only return.
Requires the current debug app already installed; restores the previous app-op.
"""
import importlib.util
import os
import signal
import sys
sys.dont_write_bytecode=True
import re
import time
import xml.etree.ElementTree as ET
from pathlib import Path

root=Path(__file__).resolve().parents[1]
(root/"tmp").mkdir(exist_ok=True)
spec=importlib.util.spec_from_file_location('h',root/'scripts/mutation-check-android-native.py')
h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
adb=[str(root/'build/android-toolchain/sdk/platform-tools/adb'),'-s',os.environ['ANDROID_SERIAL']]
def run(*args):
    value=h.run([*adb,*args],seconds=30);h.require(value);return value.stdout
def hierarchy():
    value=run('exec-out','uiautomator','dump','/dev/tty')
    return ET.fromstring(value[value.index('<?xml'):value.index('</hierarchy>')+len('</hierarchy>')])
def wait(action,seconds=25):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        try:
            value=action()
            if isinstance(value,ET.Element) or value:return value
        except (ValueError,ET.ParseError):pass
        time.sleep(.25)
    raise AssertionError('Timed out: '+run('shell','dumpsys','activity','activities')[-5000:])
def tap(text):
    node=wait(lambda:next((n for n in hierarchy().iter('node') if n.get('text')==text),None))
    x1,y1,x2,y2=map(int,re.findall(r'\d+',node.get('bounds')))
    run('shell','input','tap',str((x1+x2)//2),str((y1+y2)//2))
assert run('shell','getprop','ro.kernel.qemu').strip()=='1', 'Requires a dedicated emulator'
assert int(run('shell','getprop','ro.build.version.sdk'))>=30, 'Requires API 30+'
run('shell','run-as','com.hanxi.ghosterm','id')
def interrupted(signum,frame):raise KeyboardInterrupt(f'signal {signum}')
signal.signal(signal.SIGINT,interrupted)
signal.signal(signal.SIGTERM,interrupted)
old=run('shell','appops','get','--uid','com.hanxi.ghosterm','MANAGE_EXTERNAL_STORAGE')
match=re.search(r'MANAGE_EXTERNAL_STORAGE: (\w+)',old)
try:
    run('shell','am','force-stop','com.hanxi.ghosterm')
    run('shell','appops','set','--uid','com.hanxi.ghosterm','MANAGE_EXTERNAL_STORAGE','deny')
    run('shell','am','start','-a','android.intent.action.MAIN','-c','android.intent.category.LAUNCHER','-n','com.hanxi.ghosterm/.MainActivity')
    tap('本机文件')
    tap('浏览设备存储')
    wait(lambda:'com.android.settings/' in run('shell','dumpsys','activity','activities').split('topResumedActivity=')[-1].splitlines()[0])
    old_pid=run('shell','pidof','com.hanxi.ghosterm').strip()
    time.sleep(1)
    (root/'tmp/device-storage-before-kill.txt').write_text(run('shell','dumpsys','activity','activities'))
    run('shell','run-as','com.hanxi.ghosterm','kill','-9',old_pid)
    run('shell','appops','set','--uid','com.hanxi.ghosterm','MANAGE_EXTERNAL_STORAGE','allow')
    for _ in range(3):
        run('shell','input','keyevent','4');time.sleep(1)
        if 'com.hanxi.ghosterm/' in run('shell','dumpsys','activity','activities').split('topResumedActivity=')[-1].splitlines()[0]:break
    new_pid=wait(lambda:h.run([*adb,'shell','pidof','com.hanxi.ghosterm'],seconds=30).stdout.strip())
    tree=wait(lambda:(lambda t:t if any(n.get('text')=='本机 · 设备存储' for n in t.iter('node')) else None)(hierarchy()))
    assert old_pid!=new_pid,(old_pid,new_pid)
    (root/'tmp/device-storage-grant-return.xml').write_text(ET.tostring(tree,encoding='unicode'))
    print(f'PASS: real process death while permission UI open {old_pid} -> {new_pid}; returned to device storage without relaunching or restoring a terminal')
finally:
    run('shell','appops','set','--uid','com.hanxi.ghosterm','MANAGE_EXTERNAL_STORAGE',match[1] if match else 'default')
