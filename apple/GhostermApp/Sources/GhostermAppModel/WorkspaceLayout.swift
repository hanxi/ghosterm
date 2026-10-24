import Foundation

/// 工作区唯一布局。节点和叶子身份与原生宿主生命周期无关。
public indirect enum WorkspaceLayout: Equatable {
    case leaf(UUID)
    case split(id: UUID, axis: PaneLayout.Axis, weights: [Double], children: [WorkspaceLayout])

    public enum Edge: String, Codable { case left, right, top, bottom, center }
    public var paneIDs: [UUID] {
        switch self {
        case .leaf(let id): return [id]
        case .split(_, _, _, let children): return children.flatMap(\.paneIDs)
        }
    }
    public static func group(_ children: [WorkspaceLayout], axis: PaneLayout.Axis,
                             weights: [Double]? = nil) -> WorkspaceLayout {
        if children.count == 1 { return children[0] }
        return .split(id: UUID(), axis: axis, weights: weights ?? children.map { _ in 1 }, children: children)
    }
    public func replacing(_ target: UUID, with replacement: WorkspaceLayout) -> WorkspaceLayout {
        switch self {
        case .leaf(let id): return id == target ? replacement : self
        case .split(let id, let axis, let weights, let children):
            return .split(id: id, axis: axis, weights: weights,
                          children: children.map { $0.replacing(target, with: replacement) })
        }
    }
    public func removing(_ target: UUID, equalize: Bool = false) -> WorkspaceLayout? {
        switch self {
        case .leaf(let id): return id == target ? nil : self
        case .split(let id, let axis, let weights, let children):
            var kept: [WorkspaceLayout] = [], keptWeights: [Double] = []
            for (i, child) in children.enumerated() {
                if let node = child.removing(target, equalize: equalize) {
                    kept.append(node); keptWeights.append(weights[i])
                }
            }
            if kept.isEmpty { return nil }
            if kept.count == 1 { return kept[0] }
            return .split(id: id, axis: axis,
                          weights: equalize && paneIDs.contains(target) ? kept.map { _ in 1 } : keptWeights,
                          children: kept)
        }
    }
    public func filtering(_ allowed: Set<UUID>) -> WorkspaceLayout? {
        paneIDs.filter { !allowed.contains($0) }.reduce(Optional(self)) { $0?.removing($1) }
    }
    public func docking(source: UUID, onto target: UUID, edge: Edge) -> WorkspaceLayout? {
        guard source != target, paneIDs.contains(source), paneIDs.contains(target) else { return nil }
        if edge == .center {
            // 临时身份只用于不可观测的纯函数中间值。
            let tmp = UUID()
            return replacing(source, with: .leaf(tmp)).replacing(target, with: .leaf(source))
                .replacing(tmp, with: .leaf(target))
        }
        guard let reduced = removing(source) else { return nil }
        let before = edge == .left || edge == .top
        return reduced.replacing(target, with: Self.group(
            before ? [.leaf(source), .leaf(target)] : [.leaf(target), .leaf(source)],
            axis: edge == .left || edge == .right ? .horizontal : .vertical))
    }
    public func settingWeights(node: UUID, weights newWeights: [Double]) -> WorkspaceLayout {
        switch self {
        case .leaf: return self
        case .split(let id, let axis, let weights, let children):
            let valid = newWeights.count == children.count && newWeights.allSatisfy { $0.isFinite && $0 > 0 }
            return .split(id: id, axis: axis, weights: id == node && valid ? newWeights : weights,
                          children: children.map { $0.settingWeights(node: node, weights: newWeights) })
        }
    }
    public func minimumSize(_ size: (UUID) -> CGSize) -> CGSize {
        switch self {
        case .leaf(let id): return size(id)
        case .split(_, let axis, _, let children):
            let sizes = children.map { $0.minimumSize(size) }
            return axis == .horizontal
                ? CGSize(width: sizes.reduce(0) { $0 + $1.width }, height: sizes.map(\.height).max() ?? 0)
                : CGSize(width: sizes.map(\.width).max() ?? 0, height: sizes.reduce(0) { $0 + $1.height })
        }
    }
    public struct Divider: Equatable {
        public let node: UUID
        public let index: Int
        public let axis: PaneLayout.Axis
        public let rect: CGRect
        public let container: CGRect
        public let weights: [Double]
        public let minimums: [CGFloat]
        public let lengths: [CGFloat]
    }
    /// 在最小尺寸约束内分配剩余空间，不产生负尺寸或零尺寸。
    public func geometry(in rect: CGRect, minimum: (UUID) -> CGSize)
        -> (panes: [UUID: CGRect], dividers: [Divider]) {
        switch self {
        case .leaf(let id): return ([id: rect], [])
        case .split(let id, let axis, let weights, let children):
            let mins = children.map { $0.minimumSize(minimum) }.map { axis == .horizontal ? $0.width : $0.height }
            let length = axis == .horizontal ? rect.width : rect.height
            let total = weights.reduce(0, +)
            var lengths = weights.map { CGFloat($0 / total) * length }
            var free = Set(children.indices)
            var remaining = length
            while let i = free.first(where: { lengths[$0] < mins[$0] }) {
                lengths[i] = mins[i]; remaining -= mins[i]; free.remove(i)
                let sum = free.reduce(0.0) { $0 + weights[$1] }
                for j in free { lengths[j] = max(0, remaining) * CGFloat(weights[j] / sum) }
            }
            var offset: CGFloat = 0, frames: [UUID: CGRect] = [:], dividers: [Divider] = []
            for i in children.indices {
                let r = axis == .horizontal
                    ? CGRect(x: rect.minX + offset, y: rect.minY, width: lengths[i], height: rect.height)
                    : CGRect(x: rect.minX, y: rect.minY + offset, width: rect.width, height: lengths[i])
                let sub = children[i].geometry(in: r, minimum: minimum)
                frames.merge(sub.panes) { _, new in new }; dividers += sub.dividers
                offset += lengths[i]
                if i < children.count - 1 {
                    let line = axis == .horizontal
                        ? CGRect(x: rect.minX + offset - 3, y: rect.minY, width: 6, height: rect.height)
                        : CGRect(x: rect.minX, y: rect.minY + offset - 3, width: rect.width, height: 6)
                    dividers.append(Divider(node: id, index: i, axis: axis, rect: line,
                                            container: rect, weights: weights, minimums: mins, lengths: lengths))
                }
            }
            return (frames, dividers)
        }
    }
}

/// 窗格拖动落点几何：给定拖动点与各窗格矩形，算出目标窗格、停靠边与预览矩形。
/// 纯函数，视图只负责取点与画预览（四边/中央的判定可以直接断言）。
public enum WorkspaceDockingGeometry {
    /// 距边小于该比例 → 停靠到那条边；否则中央交换。
    public static let edgeFraction: CGFloat = 0.25
    /// 预览矩形相对目标窗格的内缩（不盖住窗格边框）。
    public static let previewInset: CGFloat = 6

    public static func resolve(point: CGPoint, frames: [(UUID, CGRect)],
                               excluding: UUID? = nil)
        -> (target: UUID, edge: WorkspaceLayout.Edge, highlight: CGRect)? {
        guard let (target, frame) = frames.first(where: { id, frame in
            id != excluding && frame.width > 0 && frame.height > 0 && frame.contains(point)
        }) else { return nil }
        let x = (point.x - frame.minX) / frame.width
        let y = (point.y - frame.minY) / frame.height
        let distances: [(CGFloat, WorkspaceLayout.Edge)] =
            [(x, .left), (1 - x, .right), (y, .top), (1 - y, .bottom)]
        let closest = distances.min { $0.0 < $1.0 }!
        let edge: WorkspaceLayout.Edge = closest.0 < edgeFraction ? closest.1 : .center
        var highlight = frame
        switch edge {
        case .left: highlight.size.width /= 2
        case .right: highlight.origin.x += frame.width / 2; highlight.size.width /= 2
        case .top: highlight.size.height /= 2
        case .bottom: highlight.origin.y += frame.height / 2; highlight.size.height /= 2
        case .center: break
        }
        return (target, edge, highlight.insetBy(dx: previewInset, dy: previewInset))
    }
}

/// 文件端点身份不等于主机显示名，也不等于窗格位置。
@MainActor public final class WorkspaceFilePane: ObservableObject, Identifiable {
    public let id: UUID
    public let endpointID: UUID
    public let remote: Bool
    @Published public private(set) var panel: FilesPanelModel?
    @Published public private(set) var error: String?
    /// 文件窗格标题条以下、内容区以上的实测高度（导航工具条 + 路径行 +
    /// 分隔线 + 搜索栏）。nil = 还没渲染过，调用方用保守常量兜底。
    @Published public private(set) var chromeHeight: CGFloat?
    public init(id: UUID = UUID(), endpointID: UUID = UUID(), remote: Bool, panel: FilesPanelModel? = nil) {
        self.id = id; self.endpointID = endpointID; self.remote = remote; self.panel = panel
    }
    public func attach(_ panel: FilesPanelModel) { self.panel = panel; error = nil }
    public func fail(_ message: String) { error = message }
    public func beginConnection() { error = nil }
    /// 只接受真实高度；亚像素抖动不发布，避免布局回路。
    public func reportChromeHeight(_ height: CGFloat) {
        guard height > 0 else { return }
        if let old = chromeHeight, abs(old - height) < 0.5 { return }
        chromeHeight = height
    }
}

@MainActor public enum WorkspacePane {
    case terminal(TerminalPane)
    case files(WorkspaceFilePane)
    public var id: UUID {
        switch self { case .terminal(let p): return p.id; case .files(let p): return p.id }
    }
    public var terminal: TerminalPane? { if case .terminal(let p) = self { return p }; return nil }
    public var files: WorkspaceFilePane? { if case .files(let p) = self { return p }; return nil }
}

public struct WorkspacePaneDrag: Codable {
    public let tabID: UUID
    public let paneID: UUID
    public init(tabID: UUID, paneID: UUID) { self.tabID = tabID; self.paneID = paneID }
}
