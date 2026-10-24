// TransferQueueView — 全局传输队列窗口（S4）。
//
// 跨标签聚合所有上传/下载：进度、速度/ETA、暂停/继续/取消/重试。
import AppKit
import GhostermAppModel
import SwiftUI

struct TransferQueueView: View {
    @ObservedObject var manager: TransferManager

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            if manager.jobs.isEmpty {
                Spacer()
                Text("没有传输").foregroundStyle(.secondary).font(DS.Font.callout)
                Spacer()
            } else {
                List {
                    ForEach(manager.jobs) { job in
                        row(job)
                    }
                }
                .listStyle(.inset)
            }
            Divider()
            footer
        }
        .frame(minWidth: 460, minHeight: 320)
    }

    private var header: some View {
        HStack {
            Text("传输").font(DS.Font.title)
            Spacer()
            Text("\(manager.activeCount) 进行中").font(DS.Font.caption)
                .foregroundStyle(.secondary)
        }
        .padding(DS.Padding.m)
    }

    private var footer: some View {
        HStack {
            Button("清除已完成") { manager.clearFinished() }
                .disabled(!manager.jobs.contains { $0.status.isTerminal })
            Spacer()
        }
        .padding(DS.Padding.s)
    }

    private func row(_ job: TransferManager.Job) -> some View {
        VStack(alignment: .leading, spacing: DS.Padding.xs) {
            HStack(spacing: DS.Padding.s) {
                Image(systemName: job.direction == .download
                      ? "arrow.down.circle" : "arrow.up.circle")
                Text(job.name).font(DS.Font.callout).lineLimit(1)
                Spacer()
                statusView(job)
            }
            if !job.status.isTerminal || job.status == .done {
                ProgressView(value: job.total > 0 ? Double(job.done) / Double(job.total) : 0)
                    .controlSize(.small)
                HStack(spacing: DS.Padding.m) {
                    Text(byteText(job.done) + (job.total > 0 ? " / " + byteText(job.total) : ""))
                    if job.speed > 0 { Text(byteText(UInt64(job.speed)) + "/s") }
                    if let eta = job.eta { Text("剩余 \(Int(eta))s") }
                }
                .font(DS.Font.captionSmall).foregroundStyle(.secondary)
            }
            // 行内控制
            HStack(spacing: DS.Padding.xs) {
                if job.status == .running {
                    Button("暂停") { manager.pause(job) }.buttonStyle(.link)
                    Button("取消") { manager.cancel(job) }.buttonStyle(.link)
                } else if job.status == .queued {
                    Button("取消") { manager.cancel(job) }.buttonStyle(.link)
                } else if job.status == .paused {
                    Button("继续") { manager.resume(job) }.buttonStyle(.link)
                    Button("取消") { manager.cancel(job) }.buttonStyle(.link)
                } else if job.status.isTerminal &&
                            job.status != .done && job.status != .skipped {
                    Button("重试") { manager.retry(job) }.buttonStyle(.link)
                }
            }
            .font(DS.Font.caption)
        }
        .padding(.vertical, DS.Padding.xs)
    }

    @ViewBuilder private func statusView(_ job: TransferManager.Job) -> some View {
        switch job.status {
        case .queued: Text("排队中").font(DS.Font.caption).foregroundStyle(.secondary)
        case .running: Text("传输中").font(DS.Font.caption).foregroundStyle(.secondary)
        case .paused: Text("已暂停").font(DS.Font.caption).foregroundStyle(.orange)
        case .done: Label("完成", systemImage: "checkmark.circle")
            .font(DS.Font.caption).foregroundStyle(.green)
        case .skipped: Text("已跳过").font(DS.Font.caption).foregroundStyle(.secondary)
        case .cancelled: Text("已取消").font(DS.Font.caption).foregroundStyle(.orange)
        case .failed(let m): Label("失败", systemImage: "exclamationmark.triangle")
            .font(DS.Font.caption).foregroundStyle(.red).help(m)
        }
    }

    private func byteText(_ n: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(n), countStyle: .file)
    }
}

/// 传输窗口的宿主（独立 NSWindow）。单例，菜单「窗口 → 传输」唤起。
@MainActor
public final class TransferWindow {
    private static var controller: NSWindowController?

    public static func open(manager: TransferManager) {
        if let c = controller { c.showWindow(nil); c.window?.makeKeyAndOrderFront(nil); return }
        let hosting = NSHostingController(rootView: TransferQueueView(manager: manager))
        let win = NSWindow(contentViewController: hosting)
        win.title = "传输"
        win.setContentSize(NSSize(width: 520, height: 380))
        win.styleMask = [.titled, .closable, .resizable, .miniaturizable]
        win.isReleasedWhenClosed = false
        let c = NSWindowController(window: win)
        controller = c
        c.showWindow(nil)
    }
}
