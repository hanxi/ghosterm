import GhostermAppModel
import SwiftUI

struct PluginTaskView: View {
    @ObservedObject var model: PluginTaskModel
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(model.descriptor?.title ?? "任务").font(.headline).accessibilityAddTraits(.isHeader)
            if let issue = model.issue { Text(issue).foregroundStyle(.red).textSelection(.enabled).accessibilityIdentifier("plugin-task-issue") }
            if let state = model.snapshot {
                if model.issue != nil { Text("以下为上次读取的任务状态").font(.caption).foregroundStyle(.secondary) }
                Text(state.phase.label).accessibilityIdentifier("plugin-task-phase")
                Text(state.message).textSelection(.enabled).accessibilityIdentifier("plugin-task-message")
                if let total = state.total {
                    Text("\(state.completed) / \(total) · \(model.descriptor?.unit ?? "")")
                        .monospacedDigit().textSelection(.enabled).accessibilityIdentifier("plugin-task-count")
                    if let maximum = UInt64(total), maximum > 0, let completed = UInt64(state.completed) {
                        ProgressView(value: Double(completed), total: Double(maximum))
                            .accessibilityLabel("任务进度").accessibilityValue("\(state.completed) / \(total)")
                    }
                } else {
                    Text("\(state.completed) · 总量未知 · \(model.descriptor?.unit ?? "")")
                        .monospacedDigit().accessibilityIdentifier("plugin-task-count")
                }
                if let result = state.result.text { Text(result).textSelection(.enabled).accessibilityIdentifier("plugin-task-result") }
            }
            if model.cancellation == .requested && model.snapshot?.phase.terminal != true {
                Text("取消请求已受理，仍需任务确认停止").accessibilityIdentifier("plugin-task-cancel-status")
            } else if model.cancellation == .unconfirmed {
                Text("取消结果不明；不会重发取消请求").accessibilityIdentifier("plugin-task-cancel-status")
            } else if model.cancellation == .alreadyFinished {
                Text("取消到达时任务已结束").accessibilityIdentifier("plugin-task-cancel-status")
            }
            if model.busy { ProgressView("正在读取任务…").accessibilityIdentifier("plugin-task-loading") }
            ViewThatFits(in: .horizontal) { HStack { controls }; VStack(alignment: .leading) { controls } }
        }.accessibilityElement(children: .contain).accessibilityIdentifier("plugin-task")
            .task(id: model.available) {
                if model.descriptor == nil && model.issue == nil { await model.load() }
                while !Task.isCancelled && model.available {
                    do { try await Task.sleep(for: .milliseconds(500)) } catch { break }
                    if model.canAutoRefresh { await model.load() }
                }
            }
    }
    @ViewBuilder private var controls: some View {
        Button("读取任务状态") { Task { await model.load() } }
            .disabled(!model.available || model.busy).accessibilityIdentifier("plugin-task-read")
        Button("取消任务") { Task { await model.cancel() } }
            .disabled(!model.canCancel).accessibilityIdentifier("plugin-task-cancel")
    }
}
