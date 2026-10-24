// ThemePicker — 配色色块（按钮旁边那一小块预览）。
//
// 取法参照 iGhostVT 的 `ThemePreviewImage`（它给每一项配色画一张小图：
// `$_` 色块 + 八色调色板条）。**色块不能进 `Menu`：条目和 label 都被 AppKit 吃**
// ——条目会被压成真 `NSMenuItem`（实测塌成字面量 `$_`，配色名全丢）；label 也会
// 被收编成按钮内容、只认一个视图（多给一个兄弟视图，色块整个不画）。
// 两处都踩过，所以色块只能当 `Menu` 的**兄弟**、放在它旁边。iGhostVT 在
// Mac/Catalyst 上踩的是同一条，它的注释原话是「AppKit draws no image on a
// Catalyst popup menu's items, so they cannot be in it」，做法也是把色块挪到
// 按钮旁边。
import GhostermAppModel
import SwiftUI

/// 一套配色的迷你预览：底色块（带 `$_` 提示符）+ 右侧八色条。
///
/// **只放在按钮旁边**，不放菜单项里（见文件头）。
struct ThemeSwatch: View {
    let preview: ThemePreview

    private var background: Color {
        ThemePreview.rgba(preview.background).map {
            Color(.sRGB, red: $0.r, green: $0.g, blue: $0.b, opacity: $0.a)
        } ?? Color.secondary.opacity(0.15)
    }

    private var foreground: Color {
        ThemePreview.rgba(preview.foreground).map {
            Color(.sRGB, red: $0.r, green: $0.g, blue: $0.b, opacity: $0.a)
        } ?? Color.primary
    }

    var body: some View {
        HStack(spacing: DS.Padding.s) {
            ZStack {
                RoundedRectangle(cornerRadius: DS.Radius.s)
                    .fill(background)
                Text(verbatim: "$_")
                    .font(.system(size: 9, weight: .bold, design: .monospaced))
                    .foregroundStyle(foreground)
            }
            .frame(width: 34, height: 22)
            .overlay(RoundedRectangle(cornerRadius: DS.Radius.s)
                .strokeBorder(Color.primary.opacity(0.15), lineWidth: 1))

            HStack(spacing: 0) {
                ForEach(0..<8, id: \.self) { i in
                    let c = ThemePreview.rgba(preview.palette[i])
                    Rectangle().fill(c.map {
                        Color(.sRGB, red: $0.r, green: $0.g, blue: $0.b, opacity: 1)
                    } ?? foreground)
                }
            }
            .frame(width: 56, height: 14)
            .clipShape(RoundedRectangle(cornerRadius: 3))
            .overlay(RoundedRectangle(cornerRadius: 3)
                .strokeBorder(Color.primary.opacity(0.15), lineWidth: 1))
        }
    }
}