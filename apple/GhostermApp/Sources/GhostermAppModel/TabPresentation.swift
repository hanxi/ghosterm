import Foundation

/// 两种导航展示共用 TabManager，偏好只决定布局，不持有标签或会话。
public enum TabPlacement: String, CaseIterable, Identifiable {
    case sidebar
    case top

    public var id: Self { self }
    public var title: String { self == .sidebar ? "侧边" : "顶部" }
}
