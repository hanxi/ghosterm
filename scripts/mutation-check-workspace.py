#!/usr/bin/env python3
"""统一工作区变异验证；只修改隔离副本，目标红且对照绿，再核对复原 SHA256。"""
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PKG = ROOT / "apple/GhostermApp"
MARK = "GT_MUTATION_MARK"
MODEL = "Sources/GhostermAppModel/"
UI = "Sources/GhostermApp/"
CASES = [
    ("保留既有终端分屏", MODEL + "TerminalTab.swift",
     "        layoutCustomized = true\n        activePaneID = newPane.id",
     "        layoutCustomized = false\n        activePaneID = newPane.id",
     "WorkspaceTests/testOpeningFilesDoesNotResetExistingTerminalSplits",
     "WorkspaceTests/testDefaultAAndDockingPreserveAllPaneIdentities"),
    ("文件分屏目录继承", MODEL + "WorkspaceTab.swift",
     "FilesPanelModel(backend: panel.backend, initialPath: panel.currentPath)", "FilesPanelModel(backend: panel.backend)",
     "WorkspaceTests/testHideRestoreAndDuplicateFilesAreIndependent",
     "WorkspaceTests/testDefaultAAndDockingPreserveAllPaneIdentities"),
    ("窗格拖动 move 掩码", UI + "WorkspaceDragSource.swift",
     "context == .withinApplication ? .move : []", "context == .withinApplication ? .copy : []",
     "WorkspaceTests/testWindowPaneDragUsesOwnTypeAndMoveMask",
     "WorkspaceTests/testDefaultAAndDockingPreserveAllPaneIdentities"),
    ("源端点身份", MODEL + "WorkspaceFiles.swift",
     "$0.id == drag.paneID && $0.endpointID == drag.endpointID", "$0.remote == true",
     "WorkspaceTests/testEndpointRoutingRejectsStaleSourceAndDoesNotUseTargetBackend",
     "WorkspaceTests/testHideRestoreAndDuplicateFilesAreIndependent"),
    ("失效源拒绝", MODEL + "WorkspaceFiles.swift",
     "let source = source(drag, in: tab)", "let source = tab.remoteFiles",
     "WorkspaceTests/testEndpointRoutingRejectsStaleSourceAndDoesNotUseTargetBackend",
     "WorkspaceTests/testHideRestoreAndDuplicateFilesAreIndependent"),
    ("跨标签页拒绝", MODEL + "WorkspaceTab.swift",
     "drag.tabID == id, let next", "true, let next",
     "WorkspaceTests/testDefaultAAndDockingPreserveAllPaneIdentities",
     "WorkspaceTests/testRetryNeverBorrowsAnotherEndpoint"),
    ("宿主身份", UI + "WorkspaceView.swift",
     "if let host = hosts[pane.id] { host.rootView = root }",
     "if let host = hosts.removeValue(forKey: pane.id) { host.removeFromSuperview() }; if false {}",
     "WorkspaceTests/testNativeHostIdentityAndSiblingGeometrySurviveMovesAndZoom",
     "WorkspaceTests/testEndpointRoutingRejectsStaleSourceAndDoesNotUseTargetBackend"),
    ("缩放兄弟尺寸", UI + "WorkspaceView.swift",
     "                host.frame = frame", "                host.frame = tab.zoomedPaneID != nil && tab.zoomedPaneID != id ? CGRect(x: 0, y: 0, width: 1, height: 1) : frame",
     "WorkspaceTests/testNativeHostIdentityAndSiblingGeometrySurviveMovesAndZoom",
     "WorkspaceTests/testDefaultAAndDockingPreserveAllPaneIdentities"),
    ("文件关闭范围", MODEL + "WorkspaceTab.swift",
     "workspacePanes.removeAll { $0.id == id }", "workspacePanes.removeAll()",
     "WorkspaceTests/testHideRestoreAndDuplicateFilesAreIndependent",
     "WorkspaceTests/testDefaultAAndDockingPreserveAllPaneIdentities"),
    ("完成回调", MODEL + "TransferManager.swift",
     "callback?(status)", "_ = callback",
     "FilesDragDropTests/testOnFinishFires",
     "WorkspaceTests/testDefaultAAndDockingPreserveAllPaneIdentities"),
    ("文件最小高度用实测头部", UI + "WorkspaceView.swift",
     "let chrome = file.chromeHeight ?? Self.fallbackFileChrome",
     "let chrome = Self.fallbackFileChrome",
     "WorkspaceTests/testFilePaneMinimumHeightTracksMeasuredChrome",
     "WorkspaceTests/testDefaultAAndDockingPreserveAllPaneIdentities"),
    ("拖动落点中央判定", MODEL + "WorkspaceLayout.swift",
     "let edge: WorkspaceLayout.Edge = closest.0 < edgeFraction ? closest.1 : .center",
     "let edge: WorkspaceLayout.Edge = closest.0 < edgeFraction ? .center : closest.1",
     "WorkspaceTests/testDockingGeometryResolvesEdgesAndCenter",
     "WorkspaceTests/testCancelledPaneDragClearsPreview"),
    ("拖动取消清预览", UI + "WorkspaceDragSource.swift",
     "NotificationCenter.default.post(name: .workspacePaneDragEnded, object: nil)",
     "_ = 0",
     "WorkspaceTests/testCancelledPaneDragClearsPreview",
     "WorkspaceTests/testDockingGeometryResolvesEdgesAndCenter"),
    ("文件拿键盘", UI + "WorkspaceView.swift",
     "        needsLayout = true\n        claimFilePaneFocus()",
     "        needsLayout = true",
     "WorkspaceTests/testFileAndTerminalFocusRoundTripAndBackgroundDoesNotSteal",
     "WorkspaceTests/testNativeHostIdentityAndSiblingGeometrySurviveMovesAndZoom"),
    ("首帧文件键盘延迟接管", UI + "WorkspaceView.swift",
     "            guard let self, let host, host === self.focusedFileHost(window: host.window) else { return }",
     "            guard let self, let host, host === self.focusedFileHost(window: host.window), false else { return }",
     "WorkspaceTests/testPureFileTabClaimsKeyboardWhenWindowIsFirstResponder",
     "WorkspaceTests/testFileAndTerminalFocusRoundTripAndBackgroundDoesNotSteal"),
    ("后台标签不抢键盘", UI + "WorkspaceView.swift",
     "let focused = isActive && tab.activeWorkspacePaneID == pane.id",
     "let focused = tab.activeWorkspacePaneID == pane.id",
     "WorkspaceTests/testFileAndTerminalFocusRoundTripAndBackgroundDoesNotSteal",
     "WorkspaceTests/testNativeHostIdentityAndSiblingGeometrySurviveMovesAndZoom"),
    ("标题栏带内不移窗", UI + "WorkspaceDragSource.swift",
     "override var mouseDownCanMoveWindow: Bool { false }",
     "override var mouseDownCanMoveWindow: Bool { true }",
     "WorkspaceTests/testWorkspaceSubtreeRefusesWindowDragInTitleBarStrip",
     "WorkspaceTests/testWindowPaneDragUsesOwnTypeAndMoveMask"),
    ("双击通道装机", UI + "FilesPanelView.swift",
     "guard let table = Self.enclosingTable(of: nsView),",
     "guard let table = Self.enclosingTable(of: nsView), false,",
     "WorkspaceTests/testFilePaneSelectionSyncKeyboardAndDoubleClickChannel",
     "WorkspaceTests/testPureFileTabClaimsKeyboardWhenWindowIsFirstResponder"),
    ("错误行偏移映射", UI + "FilesPanelView.swift",
     "let idx = clickedRow - (errorVisible ? 1 : 0)",
     "let idx = clickedRow",
     "WorkspaceTests/testFilePaneSelectionSyncKeyboardAndDoubleClickChannel",
     "WorkspaceTests/testFileAndTerminalFocusRoundTripAndBackgroundDoesNotSteal"),
    ("tag 绑定回流", UI + "FilesPanelView.swift",
     "row(e).tag(e.path)",
     "row(e).tag(e.path + \"#\")",
     "WorkspaceTests/testFilePaneSelectionSyncKeyboardAndDoubleClickChannel",
     "WorkspaceTests/testSyntheticClicksCannotDriveSwiftUIListSelection"),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f"收到信号 {signum}，复原隔离副本")
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    targets = {PKG / case[1] for case in CASES}
    for path in targets:
        if MARK in path.read_text():
            raise SystemExit(f"残留变异标记，拒绝起跑：{path}")
    root_hashes = {path: sha(path) for path in targets}
    existing = os.environ.get("GHOSTERM_MUTATION_WORK")
    work = Path(existing).resolve() if existing else Path(tempfile.mkdtemp(prefix="workspace-mutations-", dir=ROOT / "tmp"))
    if existing and (ROOT / "tmp").resolve() not in work.parents:
        raise SystemExit("复用验证目录必须在工程 tmp 内")
    shutil.copytree(ROOT / "apple", work / "apple", dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns(".build", ".swiftpm"))
    for name in ("core", "build", "scripts", "vendor", "tmp"):
        link = work / name
        if not link.exists():
            link.symlink_to(ROOT / name, target_is_directory=True)
    package = work / "apple/GhostermApp"
    env = dict(os.environ, TMPDIR=str(ROOT / "tmp") + "/")

    def run(label, test):
        log = work / (label + ".log")
        with log.open("w") as output:
            result = subprocess.run(["swift", "test", "--package-path", str(package), "--filter", test],
                                    env=env, stdout=output, stderr=subprocess.STDOUT, timeout=420)
        text = log.read_text()
        # 编译失败、超时和零测试不能用作目标变红的证据。
        if "Executed " not in text:
            raise RuntimeError(f"未完成实际测试：{log}")
        return result.returncode == 0

    if not run("baseline", "WorkspaceTests|FilesDragDropTests"):
        raise SystemExit(f"基线失败：{work}")
    for index, (name, rel, old, new, target, control) in enumerate(CASES):
        path = package / rel
        before = path.read_bytes()
        text = before.decode()
        if text.count(old) != 1:
            raise RuntimeError(f"{name} 锚点不唯一：{old}")
        try:
            path.write_text(text.replace(old, new + " /* " + MARK + " */", 1))
            if run(f"{index}-target", target):
                raise RuntimeError(f"{name} 目标假绿：{work}")
            if not run(f"{index}-control", control):
                raise RuntimeError(f"{name} 对照也红：{work}")
        finally:
            path.write_bytes(before)
            if sha(path) != hashlib.sha256(before).hexdigest():
                raise RuntimeError(f"{name} 隔离副本复原失败")
        print(f"PASS {name}：目标红、对照绿、SHA256 复原", flush=True)
    if not run("restored", "WorkspaceTests|FilesDragDropTests"):
        raise RuntimeError(f"回滚未复绿：{work}")
    if any(sha(path) != digest for path, digest in root_hashes.items()):
        raise RuntimeError("原工程发生并发改动，不能认证本轮验证")
    print(f"WORKSPACE MUTATIONS PASS；日志：{work}")


if __name__ == "__main__":
    main()
