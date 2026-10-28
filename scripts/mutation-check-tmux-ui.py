#!/usr/bin/env python3
"""Native product chooser mutations, using real tmux and a real surface."""
import hashlib, os, pathlib, shutil, signal, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parent.parent
MARK='GT_TMUX_UI_MUTATION_MARK'
FILES=['apple/GhostermApp/Sources/GhostermAppModel/TmuxChooser.swift',
       'apple/GhostermApp/Sources/GhostermAppModel/TerminalTab.swift',
       'apple/GhostermTerminal/Sources/GhostermTerminal/TerminalSurfaceView.swift']

def run(args):
    result=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require tmux-ui; shift; gt_timeout 240 "$@"','tmux-ui',str(ROOT)]+list(map(str,args)),cwd=ROOT,capture_output=True,text=True)
    return result.returncode,result.stdout+result.stderr

def main():
    os.environ.update(DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',TMPDIR=str(ROOT/'tmp'))
    original={f:(ROOT/f).read_bytes() for f in FILES};digest={f:hashlib.sha256(b).digest() for f,b in original.items()}
    if any(MARK.encode() in b for b in original.values()):raise SystemExit(2)
    def interrupt(sig,frame):raise InterruptedError(sig)
    for sig in [signal.SIGINT,signal.SIGTERM]:signal.signal(sig,interrupt)
    with tempfile.TemporaryDirectory(prefix='tmux-ui-mutations-',dir=ROOT/'tmp') as work:
        folder=pathlib.Path(work);(folder/'apple').mkdir()
        for name in ['GhostermApp','GhostermCore','GhostermTerminal']:
            shutil.copytree(ROOT/'apple'/name,folder/'apple'/name,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
        for name in ['core','terminal','build','assets']:(folder/name).symlink_to(ROOT/name,target_is_directory=True)
        package=folder/'apple/GhostermApp'
        def check(red=False,control=False):
            code,out=run(['xcrun','swift','test','--package-path',package,'--filter','HostListTests' if control else 'TmuxUITests'])
            valid=(code==1 and ('XCTAssert' in out or ': failed - ' in out) and 'Build complete' in out) if red else code==0 and 'Test Suite' in out and 'skipped' not in out
            if not valid:print(out,flush=True);raise RuntimeError('chooser mutation expected '+('red' if red else 'green'))
        check();check(control=True)
        chooser, pane, view=FILES
        mutants=[
            ('probe-error-state',chooser,'if case GhostermError.status(2, _) = error {','if case GhostermError.status(7, _) = error {'),
            ('overwrite',chooser,'} else { pane = TerminalPane() }','} else { pane = self.activePane ?? TerminalPane() }'),
            ('detach-loses-pane',chooser,'pane.onExit = nil','pane.onExit = { [weak self] pane in self?.handlePaneExit(pane) }'),
            ('close-confirm',pane,'if tmuxContext != nil || status.isExited { return false }','if status.isExited { return false }'),
            ('unknown-ui',chooser,'操作结果未知；请刷新确认，不会自动重发。','操作已成功。'),
            ('path-stale-selection',chooser,'requiresRefresh = true; selectedID = nil; sessions = []\n        message = "路径已改变，请刷新后重新选择。"','message = "路径已改变"'),
            ('close-kills-task',pane,'        onStop?()\n        session?.stop()', '        onStop?()\n        if tmuxContext != nil { try? session?.transport.write(Array("exit\\r".utf8)); Thread.sleep(forTimeInterval: 0.1) }\n        session?.stop()'),
            ('mouse',view,'        surface.sendMouseButton(.press, button: .left, modifiers: event.modifierFlags)','        _ = event.modifierFlags'),
            ('running-overwrite-guard',chooser,'if let pane, pane.isRunning && !pane.status.isExited && !pane.status.isFailed { return }','if false { return }'),
        ]
        requested = os.environ.get('GT_TMUX_UI_MUTATIONS')
        if requested: mutants = [case for case in mutants if case[0] in requested.split(',')]
        try:
            for name,file,old,new in mutants:
                source=original[file].decode();path=folder/file
                if source.count(old)!=1:raise RuntimeError('mutation source missing/ambiguous: '+name)
                path.write_text(source.replace(old,new,1)+'\n// '+MARK+'\n')
                check(red=True);check(control=True)
                path.write_bytes(original[file]);assert hashlib.sha256(path.read_bytes()).digest()==digest[file]
                check();print('PASS tmux UI '+name+': target red, control green, SHA rollback',flush=True)
        finally:
            for file,b in original.items():
                (folder/file).write_bytes(b)
                assert hashlib.sha256((ROOT/file).read_bytes()).digest()==digest[file]
    print('PASS tmux UI mutations ('+str(len(mutants))+' groups)')
if __name__=='__main__':main()
