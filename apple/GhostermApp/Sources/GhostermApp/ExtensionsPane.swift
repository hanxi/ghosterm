import GhostermAppModel
import GhostermCore
import SwiftUI
import UniformTypeIdentifiers
import CoreGraphics
import WebKit

/// Native review and explicit in-memory source decision for a fixed package.
/// This page does not claim to install, persist trust, or activate a plugin.
struct ExtensionsPane: View {
    @ObservedObject var model: AppModel
    @State private var importing = false
    @State private var entries: [Entry] = []
    @State private var issue: String?
    @State private var resourceSelection = PluginResourceSelection()

    private struct Entry: Identifiable {
        let review: ExtensionController.PackageReview
        var decision: String = "尚未批准"
        var sshAlias = ""
        var executable = "gterm"
        var executorDirectory = ""
        var executorDigest = ""
        var command = ""
        var argumentsJSON = "[]"
        var methodsJSON = "[]"
        var hostMethodsJSON = "[]"
        var resourceRoot = ""
        init(review: ExtensionController.PackageReview) {
            self.review = review
            self.command = review.commands.first?.path ?? ""
        }
        var id: String { review.id + ":" + review.digest }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.m) {
            HStack {
                Text("插件包").font(DS.Font.labelEmphasis)
                Spacer()
                Button("选择文件夹…") { importing = true }
                    .disabled(model.extensionController == nil)
            }

            if model.extensionController == nil {
                Label("此构建未启用 Portable JS 插件运行时。", systemImage: "exclamationmark.circle")
                    .foregroundStyle(.secondary)
                SettingsNote("普通 Ghosterm 构建默认关闭插件运行时；当前不能审查或启动 Client 插件。")
            } else if entries.isEmpty {
                SettingsNote("选择插件包文件夹后，Ghosterm 会检查固定内容摘要、兼容性和命令。审查不会自动批准或启动插件。")
            }

            if let issue {
                Text(issue).foregroundStyle(.red).textSelection(.enabled)
            }

            ForEach($entries) { $entry in
                VStack(alignment: .leading, spacing: DS.Padding.s) {
                    HStack(alignment: .firstTextBaseline) {
                        Text(entry.review.id).font(DS.Font.labelEmphasis)
                        Spacer()
                        Text(entry.decision).foregroundStyle(.secondary)
                    }
                    Text("版本 \(entry.review.version) · 发布者 \(entry.review.publisher) · \(entry.review.license)")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    Text("运行时：\(entry.review.runtime) · 发布者身份：\(entry.review.publisherAuthenticated ? "已认证" : "未认证")")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    Text("声明的必需能力：\(entry.review.requiredCapabilities.isEmpty ? "无" : entry.review.requiredCapabilities.joined(separator: ", "))")
                        .font(DS.Font.caption)
                    Text("缺失的可选能力：\(entry.review.optionalMissingCapabilities.isEmpty ? "无" : entry.review.optionalMissingCapabilities.joined(separator: ", "))")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    Text("摘要：\(entry.review.digest)").font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                    Text("命令：\(entry.review.commands.map(\.path).joined(separator: ", "))")
                        .font(DS.Font.caption)
                    Text("能力授权：\(entry.review.authorizationGranted ? "已授予" : "未授予")；来源批准不会授予能力。")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    if entry.decision == "已批准（本进程）" {
                        VStack(alignment: .leading, spacing: DS.Padding.s) {
                            TextField("SSH 主机别名", text: $entry.sshAlias)
                            TextField("远端 gterm 可执行文件", text: $entry.executable)
                            TextField("Executor 包目录", text: $entry.executorDirectory)
                            TextField("Executor SHA-256", text: $entry.executorDigest)
                                .font(.system(size: 11, design: .monospaced))
                            TextField("完整命令路径", text: $entry.command)
                            TextField("命令参数 JSON 字符串数组", text: $entry.argumentsJSON)
                            TextField("Executor RPC allowlist JSON", text: $entry.methodsJSON)
                            TextField("Host RPC allowlist JSON", text: $entry.hostMethodsJSON)
                            TextField("远端资源目录（留空则不开放）", text: $entry.resourceRoot)
                            Button("连接并启动 Client 命令") { launch(entry) }
                                .disabled(model.extensionRunBusy || model.extensionRun != nil)
                        }
                    } else {
                        HStack {
                            Button("批准此摘要") { decide(entry: entry, approve: true) }
                                .disabled(entry.decision == "已批准（本进程）")
                            Button("拒绝") { decide(entry: entry, approve: false) }
                        }
                    }
                }
                .padding(DS.Padding.m)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(.quaternary, in: RoundedRectangle(cornerRadius: DS.Radius.control))
            }

            if let run = model.extensionRun { activeRun(run) }
            if let message = model.extensionRunIssue {
                Text(message).foregroundStyle(.red).textSelection(.enabled)
            }
            SettingsNote("批准仅作用于当前 AppModel 生命周期中的精确包摘要；安装与持久信任尚未接入。资源面板支持有界 UTF-8、Markdown、静态图片和受限 HTML 预览；文档图片只使用本次显式收到的同目录附件。")
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.folder], allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls):
                guard let url = urls.first else { return }
                Task { await review(url) }
            case .failure(let error):
                issue = "无法选择插件包：\(error.localizedDescription)"
            }
        }
    }

    private func review(_ url: URL) async {
        guard let controller = model.extensionController else { return }
        issue = nil
        do {
            let value = try await controller.review(directory: url)
            guard !entries.contains(where: { $0.id == value.id + ":" + value.digest }) else { return }
            entries.append(Entry(review: value))
        } catch {
            issue = "插件包审查失败：\(error)"
        }
    }

    private func decide(entry: Entry, approve: Bool) {
        guard let controller = model.extensionController else { return }
        Task {
            do {
                let status = approve
                    ? try await controller.approve(id: entry.review.id, digest: entry.review.digest)
                    : try await controller.deny(id: entry.review.id, digest: entry.review.digest)
                guard let index = entries.firstIndex(where: { $0.id == entry.id }) else { return }
                entries[index].decision = status == 0 ? (approve ? "已批准（本进程）" : "已拒绝") : "裁决失败（\(status)）"
            } catch {
                issue = "无法保存来源裁决：\(error)"
            }
        }
    }

    private func launch(_ entry: Entry) {
        do {
            let arguments = try ExtensionRelayBootstrap.parseArgumentsJSON(entry.argumentsJSON)
            let methods = try ExtensionRelayBootstrap.parseMethodsJSON(entry.methodsJSON)
            let hostMethods = try ExtensionRelayBootstrap.parseHostMethodsJSON(entry.hostMethodsJSON)
            let roots: [ExtensionRelayBootstrap.ScopedRoot] = entry.resourceRoot.isEmpty
                ? [] : [.init(id: "workspace", directory: entry.resourceRoot)]
            guard !entry.sshAlias.isEmpty else { throw GhostermCore.GhostermError.failed("请填写 SSH 主机别名") }
            Task {
                await model.startExtensionClient(packageID: entry.review.id,
                    packageDigest: entry.review.digest, command: entry.command,
                    arguments: arguments, sshAlias: entry.sshAlias,
                    executable: entry.executable, executorDirectory: entry.executorDirectory,
                    executorDigest: entry.executorDigest, methods: methods, hostMethods: hostMethods,
                    scopedRoots: roots)
            }
        } catch { issue = "插件启动参数无效：\(error)" }
    }

    @ViewBuilder
    private func activeRun(_ run: AppModel.ExtensionRun) -> some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            Text("插件运行").font(DS.Font.labelEmphasis)
            Text("\(run.packageID) · \(run.command) · \(run.peerIdentity)")
                .font(DS.Font.caption).textSelection(.enabled)
            Text("Client 摘要：\(run.packageDigest)")
                .font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
            Text("Executor 摘要：\(run.executorDigest)")
                .font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
            Text("远端启动命令：\(run.remoteCommand)")
                .font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
            Text("参数：\(run.arguments.isEmpty ? "无" : run.arguments.map { String(reflecting: $0) }.joined(separator: " "))")
                .font(DS.Font.caption).textSelection(.enabled)
            Text("按已检查 argsSchema 解码：\(run.decodedArguments ?? "无映射（空参数）")")
                .font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
            Text("Executor RPC：\(run.methods.isEmpty ? "无" : run.methods.joined(separator: ", "))")
                .font(DS.Font.caption)
            Text("Host RPC：\(run.hostMethods.isEmpty ? "无" : run.hostMethods.joined(separator: ", "))")
                .font(DS.Font.caption)
            if let session = run.session,
               session.phase == GhostermCore.ExtensionSession.Phase.consent.rawValue {
                Text("远端请求建立 Native Session。请核对上方固定包摘要、远端身份、命令、参数与本次 RPC allowlist；批准后 Client JavaScript 才会激活。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
                HStack {
                    Button("拒绝并结束") { Task { await model.decideExtensionConsent(allow: false) } }
                    Button("批准并启动") { Task { await model.decideExtensionConsent(allow: true) } }
                        .buttonStyle(.borderedProminent)
                }
                .disabled(model.extensionRunBusy)
            } else {
                if let session = run.session {
                    Text("Session phase：\(session.phase) · end：\(session.end) · cleanup：\(session.cleanupError)")
                        .font(DS.Font.caption).textSelection(.enabled)
                }
                if let runtime = run.runtime {
                    Text("Runtime phase：\(runtime.phase) · failure：\(runtime.failure) · cleanup：\(runtime.cleanupError)")
                        .font(DS.Font.caption).textSelection(.enabled)
                }
                if !run.receivedResources.isEmpty {
                    Text("本次运行收到的资源").font(DS.Font.labelEmphasis)
                    PluginResourceBrowser(resources: run.receivedResources, selection: $resourceSelection)
                }
                Button(model.extensionRunBusy ? "正在停止…" : "停止并释放插件") {
                    Task { await model.stopExtensionRun() }
                }
                .disabled(model.extensionRunBusy)
            }
        }
        .padding(DS.Padding.m)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.quaternary, in: RoundedRectangle(cornerRadius: DS.Radius.control))
        .onChange(of: model.extensionRun?.receivedResources.map(\.resourceID) ?? []) { identifiers in
            resourceSelection.reconcile(identifiers)
        }
    }
}

struct PluginResourceSelection: Equatable {
    private(set) var resourceID: String?

    mutating func select(_ resourceID: String, available: [String]) {
        guard available.contains(resourceID) else { return }
        self.resourceID = resourceID
    }

    mutating func reconcile(_ available: [String]) {
        guard let current = resourceID, available.contains(current) else {
            resourceID = available.first
            return
        }
    }
}

struct PluginResourceBrowser: View {
    let resources: [ExtensionController.ReceivedResource]
    @Binding var selection: PluginResourceSelection

    private var identifiers: [String] { resources.map(\.resourceID) }
    private var selectedResource: ExtensionController.ReceivedResource? {
        resources.first { $0.resourceID == selection.resourceID }
    }

    var body: some View {
        HStack(alignment: .top, spacing: DS.Padding.s) {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: DS.Padding.xs) {
                    ForEach(resources, id: \.resourceID) { resource in
                        Button {
                            selection.select(resource.resourceID, available: identifiers)
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(resource.displayName.isEmpty ? "未命名资源" : resource.displayName)
                                    .lineLimit(1)
                                Text("\(resource.mime) · \(resource.size) 字节")
                                    .font(DS.Font.caption).foregroundStyle(.secondary).lineLimit(1)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(DS.Padding.s)
                            .background(selection.resourceID == resource.resourceID
                                ? Color.accentColor.opacity(0.16) : Color.clear,
                                in: RoundedRectangle(cornerRadius: DS.Radius.control))
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .accessibilityIdentifier("plugin-resource-select-\(resource.resourceID)")
                    }
                }
            }
            .frame(width: 190)
            .frame(maxHeight: 360)
            if let selectedResource {
                PluginResourcePreviewCard(resource: selectedResource, resources: resources)
                    .id(selectedResource.resourceID)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .accessibilityIdentifier("plugin-resource-preview-\(selectedResource.resourceID)")
            }
        }
        .onAppear { selection.reconcile(identifiers) }
        .onChange(of: identifiers) { selection.reconcile($0) }
    }
}

private struct PluginResourcePreviewCard: View {
    let resource: ExtensionController.ReceivedResource
    let resources: [ExtensionController.ReceivedResource]

    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.xs) {
            Text(resource.displayName.isEmpty ? "未命名资源" : resource.displayName)
                .font(DS.Font.caption).textSelection(.enabled)
            Text("\(resource.mime) · \(resource.size) 字节 · SHA-256 \(resource.sha256)")
                .font(.system(size: 10, design: .monospaced)).textSelection(.enabled)
            if PluginResourceHTMLDecoder.supports(mime: resource.mime) {
                PluginResourceHTMLPreview(resource: resource, resources: resources)
            } else if resource.mime.lowercased().hasPrefix("image/") {
                PluginResourceImagePreview(data: resource.bytes, mime: resource.mime)
            } else if PluginResourceMarkdownDecoder.supports(
                mime: resource.mime, displayName: resource.displayName) {
                PluginResourceMarkdownPreview(resource: resource, resources: resources)
            } else if let preview = String(data: resource.bytes.prefix(16384), encoding: .utf8) {
                Text(preview.isEmpty ? "空文本资源" : preview)
                    .font(.system(size: 11, design: .monospaced))
                    .textSelection(.enabled).lineLimit(12)
            } else {
                Text("此资源不是有效 UTF-8 文本；当前显示摘要，不解码为图片或 HTML。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            }
        }
        .padding(DS.Padding.s)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.background, in: RoundedRectangle(cornerRadius: DS.Radius.control))
    }
}

private struct PluginResourceImagePreview: View {
    let data: Data
    let mime: String
    @State private var image: CGImage?
    @State private var rejected = false

    var body: some View {
        Group {
            if let image {
                Image(decorative: image, scale: 1)
                    .resizable().scaledToFit().frame(maxHeight: 360)
            } else if rejected {
                Text("图片格式不受支持或超过 16,777,216 像素解码上限。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            } else {
                ProgressView("检查图片…")
                    .controlSize(.small)
            }
        }
        .task(id: data) {
            let bytes = data
            let contentType = mime
            guard let decoded = await PluginResourceDecodeTask.run({
                PluginResourceImageDecoder.decode(bytes, mime: contentType)
            }) else { return }
            image = decoded
            rejected = decoded == nil
        }
    }
}

private struct PluginResourceMarkdownPreview: View {
    let resource: ExtensionController.ReceivedResource
    let resources: [ExtensionController.ReceivedResource]
    @State private var markdown: AttributedString?
    @State private var rejected = false

    var body: some View {
        Group {
            if let markdown {
                VStack(alignment: .leading, spacing: DS.Padding.xs) {
                    ForEach(segments(from: markdown)) { segment in
                        if let text = segment.text {
                            Text(text).textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        } else if let asset = segment.asset {
                            PluginResourceImagePreview(data: asset.data, mime: asset.mime)
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            } else if rejected {
                Text("Markdown 无法解析或超过 256 KiB 预览上限。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            } else {
                ProgressView("检查 Markdown…").controlSize(.small)
            }
        }
        .task(id: resource.sha256) {
            let bytes = resource.bytes
            let mime = resource.mime
            let name = resource.displayName
            let baseURL = PluginResourceHTMLAssetResolver.documentURL(name)
            guard let decoded = await PluginResourceDecodeTask.run({
                PluginResourceMarkdownDecoder.decode(bytes, mime: mime, displayName: name,
                                                     baseURL: baseURL)
            }) else { return }
            markdown = decoded
            rejected = decoded == nil
        }
    }

    private func segments(from markdown: AttributedString) -> [Segment] {
        let resolver = PluginResourceHTMLAssetResolver(document: resource, resources: resources)
        var result: [Segment] = []
        var text = AttributedString()

        func flushText() {
            guard !text.characters.isEmpty else { return }
            result.append(Segment(id: result.count, text: text, asset: nil))
            text = AttributedString()
        }

        for run in markdown.runs {
            let part = AttributedString(markdown[run.range])
            if let imageURL = run.imageURL,
               let asset = resolver.resolve(imageURL.absoluteURL), asset.mime.hasPrefix("image/") {
                flushText()
                result.append(Segment(id: result.count, text: nil,
                                      asset: PluginResourceHTMLAssetResolver.Asset(data: asset.data,
                                                                                  mime: asset.mime)))
            } else {
                text.append(part)
            }
        }
        flushText()
        return result
    }

    private struct Segment: Identifiable {
        let id: Int
        let text: AttributedString?
        let asset: PluginResourceHTMLAssetResolver.Asset?
    }
}

private struct PluginResourceHTMLPreview: View {
    let resource: ExtensionController.ReceivedResource
    let resources: [ExtensionController.ReceivedResource]
    @State private var document: String?
    @State private var rejected = false

    var body: some View {
        Group {
            if let document, PluginResourceHTMLAssetResolver.documentURL(resource.displayName) != nil {
                PluginResourceHTMLWebView(document: document, resource: resource, resources: resources)
                    .frame(minHeight: 180, maxHeight: 420)
            } else if rejected {
                Text("HTML 无法解析、路径无效或超过 256 KiB 预览上限。脚本和网络请求禁止；只加载本次显式收到的同目录 CSS / 图片。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            } else {
                ProgressView("检查 HTML…").controlSize(.small)
            }
        }
        .task(id: resource.sha256) {
            let bytes = resource.bytes
            let mime = resource.mime
            guard PluginResourceHTMLAssetResolver.documentURL(resource.displayName) != nil else {
                document = nil
                rejected = true
                return
            }
            guard let decoded = await PluginResourceDecodeTask.run({
                PluginResourceHTMLDecoder.document(bytes, mime: mime)
            }) else { return }
            document = decoded
            rejected = decoded == nil
        }
    }
}

private struct PluginResourceHTMLWebView: NSViewRepresentable {
    let document: String
    let resource: ExtensionController.ReceivedResource
    let resources: [ExtensionController.ReceivedResource]

    func makeCoordinator() -> Coordinator {
        Coordinator(resource: resource, resources: resources)
    }

    func makeNSView(context: Context) -> WKWebView {
        let configuration = WKWebViewConfiguration()
        configuration.defaultWebpagePreferences.allowsContentJavaScript = false
        configuration.preferences.javaScriptCanOpenWindowsAutomatically = false
        configuration.setURLSchemeHandler(context.coordinator.schemeHandler,
                                          forURLScheme: "ghosterm-resource")
        let webView = WKWebView(frame: .zero, configuration: configuration)
        webView.navigationDelegate = context.coordinator
        webView.uiDelegate = context.coordinator
        webView.setValue(false, forKey: "drawsBackground")
        context.coordinator.update(document: document, resource: resource, resources: resources,
                                   in: webView)
        return webView
    }

    func updateNSView(_ webView: WKWebView, context: Context) {
        context.coordinator.update(document: document, resource: resource, resources: resources,
                                   in: webView)
    }

    @MainActor
    final class Coordinator: NSObject, WKNavigationDelegate, WKUIDelegate {
        private var loadedDocument: String?
        private var resourceFingerprint = ""
        private var baseURL: URL?
        let schemeHandler: PluginResourceHTMLSchemeHandler

        init(resource: ExtensionController.ReceivedResource,
             resources: [ExtensionController.ReceivedResource]) {
            schemeHandler = PluginResourceHTMLSchemeHandler(document: resource, resources: resources)
            super.init()
        }

        func update(document: String, resource: ExtensionController.ReceivedResource,
                    resources: [ExtensionController.ReceivedResource], in webView: WKWebView) {
            let fingerprint = resources.map { "\($0.resourceID):\($0.sha256)" }.joined(separator: ",")
            if fingerprint != resourceFingerprint {
                resourceFingerprint = fingerprint
                schemeHandler.update(document: resource, resources: resources)
                if let loadedDocument, let baseURL {
                    webView.loadHTMLString(loadedDocument, baseURL: baseURL)
                }
            }
            let target = PluginResourceHTMLAssetResolver.documentURL(resource.displayName)
            guard loadedDocument != document || baseURL != target, let target else { return }
            loadedDocument = document
            baseURL = target
            webView.loadHTMLString(document, baseURL: target)
        }

        func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction,
                     decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
            if PluginResourceHTMLNavigationPolicy.permitsAction(
                type: navigationAction.navigationType,
                isMainFrame: navigationAction.targetFrame?.isMainFrame == true,
                url: navigationAction.request.url, documentURL: baseURL) {
                decisionHandler(.allow)
            } else {
                decisionHandler(.cancel)
            }
        }

        func webView(_ webView: WKWebView, decidePolicyFor navigationResponse: WKNavigationResponse,
                     decisionHandler: @escaping (WKNavigationResponsePolicy) -> Void) {
            decisionHandler(PluginResourceHTMLNavigationPolicy.permitsResponse(
                url: navigationResponse.response.url, documentURL: baseURL) ? .allow : .cancel)
        }

        func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
                     for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
            nil
        }
    }
}
