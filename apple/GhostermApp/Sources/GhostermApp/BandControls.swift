// BandControls — 顶带最左边那一组按钮：红绿灯留白 · 侧栏开关 · 新建 · ⚠。
//
// 为什么是**一份**视图给两处用（侧栏顶带 / 顶栏）：按钮的 x 必须与侧栏开合
// 无关。先前的规则是「按钮待在它要移动的那条边上」——侧栏展开时开关跑到侧
// 栏右端（x≈200），收起时回到红绿灯右侧（x≈94）。结果是同一个按钮在两种状
// 态下隔着大半个侧栏宽，开合一次就得重新找它。改成整组贴着红绿灯左对齐，
// 两种状态下 x 一模一样，手不用动。
//
// 顺序也钉死：**开关在左、`+` 在右**。开关是「这条带子自己的」控件（它决定
// 带子左边是什么），`+` 是内容区的动作；开关挨着红绿灯，与系统窗口控件连成
// 一排，`+` 挨着它右边的标签内容。⚠ 只在出错时出现，且永远挂在**最右**，
// 这样它的出现与消失不会把前面两个按钮推来推去。
import GhostermAppModel
import SwiftUI

struct BandControls: View {
    @ObservedObject var model: AppModel
    @Binding var showsSidebar: Bool
    let onShowSettings: () -> Void

    var body: some View {
        // 自己带 HStack：两处父视图都是 `HStack(spacing: DS.Padding.s)`，
        // 这组作为**第一个**子视图，父级的间距不会在它前面插空，x 才对得齐。
        HStack(spacing: DS.Padding.s) {
            // 红绿灯浮在这片留白上（它不是我们画的，是窗口自己的控件）。
            // 不留宽度第一个按钮就被压在红绿灯下面——用户截图里见过。
            Color.clear
                .frame(width: WindowChrome.trafficLightsWidth, height: 1)
                .titleBarDoubleClick()   // 双击顶栏空白处 = 系统的标题栏双击

            GlassIconButton(systemName: "sidebar.leading",
                            help: showsSidebar ? "隐藏侧边栏" : "显示侧边栏") {
                // 两个方向都由这一个按钮管。踩过的坑：先前侧栏开合各画一个
                // 按钮，收起时那个抄来的还写着 `showsSidebar = false`，按下去
                // 什么都不发生，侧栏再也打不开（用户实测）。
                showsSidebar.toggle()
            }

            // 「新建标签页」全窗口只有这一个入口（两处都画就会出现两个加号）。
            NewTabMenu(model: model)

            if let err = model.terminalConfigError {
                // 终端配置坏了必须说得出口，否则「改了字体没生效」无从查起。
                // 这句原来在侧栏底部那条 footer 上；footer 连同 ⚙︎ 一起删了，
                // 它得有个新落点，而且侧栏收起时也要看得见——以前收起就没了。
                Button(action: onShowSettings) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .frame(width: DS.controlSize, height: DS.controlSize)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .foregroundStyle(.orange)
                .help("终端配置无效：\(err)")
                .accessibilityLabel("终端配置无效")
            }
        }
    }
}
