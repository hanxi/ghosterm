#!/usr/bin/env python3
"""Literal SSH editor: compiled target red, same-APK Core green, restored green."""
import hashlib
import signal

MARK = 'GT_ANDROID_CONFIG_INPUT_MUTATION'


def execute(root, build, test):
    path = root/'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    original = path.read_bytes()
    digest = hashlib.sha256(original).digest()
    if MARK.encode() in original:
        raise RuntimeError('Residual config input mutation')
    target = 'com.hanxi.ghosterm.HostScreenTest#configEditorDisablesAutocorrectAndWritesLiteralText'
    cases = [
        ('autocorrect',
         'InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS',
         'InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or InputType.TYPE_TEXT_FLAG_AUTO_CORRECT',
         'SSH config must not request automatic correction'),
        ('literal-mode',
         'InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS',
         'InputType.TYPE_TEXT_VARIATION_NORMAL or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS',
         'SSH config must request literal input'),
        ('suggestions',
         'InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS',
         'InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD',
         'SSH config must disable input-method suggestions'),
        ('multiline',
         'OutlinedTextField(hosts.configText, { hosts.configText = it }, enabled = !hosts.busy,',
         'OutlinedTextField(hosts.configText, { hosts.configText = it }, enabled = !hosts.busy, singleLine = true,',
         'SSH config editor must retain multiline input'),
        ('literal',
         'OutlinedTextField(hosts.configText, { hosts.configText = it }, enabled = !hosts.busy,',
         'OutlinedTextField(hosts.configText, { hosts.configText = it.replace("ssh.github.com", "she.github.com") }, enabled = !hosts.busy,',
         'Literal SSH config text must reach disk unchanged'),
    ]
    for name, old, _, _ in cases:
        if original.decode().count(old) != 1:
            raise RuntimeError('Ambiguous config input mutation: '+name)
    handlers = {s: signal.getsignal(s) for s in (signal.SIGINT, signal.SIGTERM)}
    def interrupted(signum, frame):
        raise KeyboardInterrupt('Config input mutation interrupted')
    for signum in handlers:
        signal.signal(signum, interrupted)
    try:
        for name, old, new, message in cases:
            label = 'config-input-'+name
            path.write_text(original.decode().replace(old, new)+'\n// '+MARK+'\n')
            build(label)
            output = test(label, target, False)
            if target.split('#')[1] not in output or message not in output:
                raise RuntimeError('Config input mutation did not reach its ordinary target assertion: '+name)
            test(label+'-control', 'com.hanxi.ghosterm.NativeSmokeTest#corePreservesUtf8AndLineEndings', True)
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError('Config input restoration failed')
            build(label+'-restored')
            test(label+'-restored', target, True)
            print('PASS '+label+': compiled target red / same APK Core green / restored green / SHA256', flush=True)
    finally:
        if MARK.encode() in path.read_bytes():
            path.write_bytes(original)
        for signum, handler in handlers.items():
            signal.signal(signum, handler)
        if hashlib.sha256(path.read_bytes()).digest() != digest:
            raise RuntimeError('Config input source restoration failed')
    test('config-input-final', 'com.hanxi.ghosterm.HostScreenTest,com.hanxi.ghosterm.NativeSmokeTest', True)
    print('ANDROID CONFIG INPUT MUTATIONS PASS: 5 groups', flush=True)


if __name__ == '__main__':
    raise SystemExit('Run with ANDROID_CONFIG_INPUT_MUTATE=1 through test-android-ssh.sh')
