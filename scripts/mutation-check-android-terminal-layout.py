#!/usr/bin/env python3
"""Real short-window/IME controls, routing and geometry mutations with Core controls."""
import hashlib,os
MARK='GT_ANDROID_TERMINAL_LAYOUT_MUTATION'
def execute(root,build,test):
    base=root/'android/app/src/main/java/com/hanxi/ghosterm'
    screen=base/'TerminalScreen.kt';bar=base/'CompactTerminalBar.kt';system=base/'TerminalSystemBars.kt'
    originals={p:p.read_bytes() for p in (screen,bar,system)}
    if any(MARK.encode() in b for b in originals.values()):raise RuntimeError('Residual terminal layout mutation')
    default='landscapeImeKeepsRealPixelsControlsAndSamePty'
    routing='largeFontCompactMenuRoutesFilesAndSessionsWithoutReplacingOwner'
    short='shortWindowScrollsAllKeysAndKeepsMenuAboveTheWindowEdge'
    portrait='portraitFocusKeepsMenuFixedAndKeyboardInMenu'
    status='statusBarToggleAndLeavingTerminalRestoreSystemChromeWithoutReplacingPty'
    def replacement(path,old,new):
        def change():
            text=originals[path].decode();assert text.count(old)==1,(old,text.count(old));return {path:text.replace(old,new)}
        return change
    def pin_ctrl():
        text=originals[bar].decode();start=text.index('                TextButton(onClick = onCtrl,');end=text.index('                val keys =',start);block=text[start:end];text=text[:start]+text[end:]
        anchor='            Row(Modifier.weight(1f).height(48.dp).horizontalScroll'
        assert text.count(anchor)==1
        return {bar:text.replace(anchor,block+anchor)}
    def scroll_menu():
        text=originals[bar].decode();start=text.index('        Box {\n            IconButton(onClick = onMenu,');end=text.index('\n    }\n}',start);block=text[start:end];text=text[:start]+text[end:]
        anchor='                val keys ='
        assert text.count(anchor)==1
        return {bar:text.replace(anchor,block+'\n'+anchor)}
    cases=[
        ('portrait-row-height',portrait,replacement(bar,'.height(48.dp).background(MaterialTheme.colorScheme.surface)','.height(96.dp).background(MaterialTheme.colorScheme.surface)')),
        ('portrait-files-route',portrait,replacement(screen,'menu = false; terminal?.hideKeyboard(); openFiles()','menu = false; terminal?.hideKeyboard(); Unit')),
        ('portrait-sessions-route',portrait,replacement(screen,'menu = false; terminal?.hideKeyboard(); list = true','menu = false; terminal?.hideKeyboard(); Unit')),
        ('keyboard-focus',portrait,replacement(screen,'target.showKeyboard()','Unit')),
        ('legacy-chrome',portrait,replacement(screen,'Column(Modifier.fillMaxSize()) {','Column(Modifier.fillMaxSize()) { Spacer(Modifier.height(60.dp))')),
        ('status-default',status,replacement(screen,'var statusBarVisible by rememberSaveable { mutableStateOf(false) }','var statusBarVisible by rememberSaveable { mutableStateOf(true) }')),
        ('status-toggle',status,replacement(screen,'statusBarVisible = !statusBarVisible','statusBarVisible = statusBarVisible')),
        ('status-restore',status,replacement(system,'if (previousVisible) it.show(WindowInsetsCompat.Type.statusBars())','if (previousVisible) it.hide(WindowInsetsCompat.Type.statusBars())')),
        ('hidden-status-insets',portrait,replacement(screen,'systemInsets.union(WindowInsets.displayCutout)','WindowInsets.systemBars.union(WindowInsets.displayCutout)')),
        ('ime-insets',default,replacement(screen,'.union(WindowInsets.ime)','')),
        ('bar-height',default,replacement(bar,'.height(48.dp).background(MaterialTheme.colorScheme.surface)','.height(24.dp).background(MaterialTheme.colorScheme.surface)')),
        ('back-route',default,replacement(screen,'BackHandler(onBack = back)','BackHandler(onBack = {})')),
        ('keyboard-route',default,replacement(screen,'menu = false; keyboardRequested = true','menu = false; keyboardRequested = false')),
        ('files-route',routing,replacement(screen,'menu = false; terminal?.hideKeyboard(); openFiles()','menu = false; terminal?.hideKeyboard(); Unit')),
        ('sessions-route',routing,replacement(screen,'menu = false; terminal?.hideKeyboard(); list = true','menu = false; terminal?.hideKeyboard(); Unit')),
        ('menu-fixed',portrait,scroll_menu),
        ('sheet-expanded',routing,replacement(screen,'rememberModalBottomSheetState(skipPartiallyExpanded = true)','rememberModalBottomSheetState(skipPartiallyExpanded = false)')),
        ('side-controls',routing,replacement(screen,'val sideControls = compact && maxWidth >= 480.dp &&','val sideControls = false &&')),
        ('auxiliary-policy',short,replacement(bar,'if (policy.auxiliaryKeys)','if (true)')),
        ('extended-policy',short,replacement(bar,'if (policy.extendedKeys)','if (false)')),
        ('scrollable-ctrl',portrait,pin_ctrl),
        ('error-visible',short,replacement(screen,'if (compact) (model.error','if (false) (model.error')),
    ]
    selected=os.environ.get('ANDROID_TERMINAL_LAYOUT_CASES','').split(',')
    if selected!=['']:
        if set(selected)-{n for n,_,_ in cases}:raise RuntimeError('Unknown layout mutation')
        cases=[c for c in cases if c[0] in selected]
    # Check every selected source anchor before writing the first mutation.
    for _,_,change in cases:change()
    try:
        for name,method,change in cases:
            for path,text in change().items():path.write_text(text+'\n// '+MARK+'\n')
            label='terminal-layout-'+name;selection='com.hanxi.ghosterm.TerminalLayoutTest#'+method
            build(label);output=test(label,selection,False)
            if 'AssertionError' not in output and 'ComparisonFailure' not in output:raise RuntimeError('Expected a compiled normal assertion red')
            if 'Fixture terminal' in output or 'Fixture did not record' in output:raise RuntimeError('Fixture startup is not a valid layout target red')
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            for path,data in originals.items():path.write_bytes(data)
            build(label+'-restored');test(label+'-restored',selection,True)
            assert all(hashlib.sha256(p.read_bytes()).digest()==hashlib.sha256(b).digest() for p,b in originals.items())
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(p.read_bytes()!=b for p,b in originals.items()):raise RuntimeError('Layout restoration failed')
    print(f'ANDROID TERMINAL LAYOUT MUTATIONS PASS: {len(cases)} groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_TERMINAL_LAYOUT_MUTATE=1 with test-android-ssh.sh and TerminalLayoutTest')
