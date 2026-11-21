# Ghosterm 落地页

发布地址：https://hanxi.cc/ghosterm/

纯静态 HTML / CSS，无依赖或构建步骤。GitHub Pages 只上传 `site/`，不会发布仓库其它目录。`site/**` 或 Pages 工作流变更推送到 main 后自动部署，也支持手动触发。仓库 Pages 的构建来源为 GitHub Actions。

插件渠道可将签名策略的三个公开文件放入 `site/plugins/`。部署前使用仓库变量
`MACOS_PLUGIN_CHANNEL` 和 `MACOS_PLUGIN_CHANNEL_ROOT` 核对签名、代次与有效期。
部署后工作流要求公开 HTTPS 地址直接返回 HTTP 200，并逐字节比对三个文件。
复验通过后可使用 `https://hanxi.cc/ghosterm/plugins/` 配置应用出包的渠道地址。
渠道签发和撤销流程见[插件开发指南](../docs/how-to/plugin-development.md)。

本地预览（工程根目录执行）：

```bash
python3 -m http.server 8080 --directory site
```

打开 http://localhost:8080 。校验 HTML 的本地链接和资源、移动端布局与下载地址后提交。下载统一指向滚动 dev 资产，不在页面写死构建号、测试数量或包大小；平台能力以 `docs/reference/current-status.md` 和能力矩阵为准。

`assets/screenshots.json` 记录三张实际原生应用截图的来源及 SHA256。截图含隔离测试数据，按原图保存，没有使用 HTML 原型冒充产品截图；图注保留平台、历史界面和测试环境边界。图标来自 `assets/icons/ghosterm-mark.svg`，更新须通过仓库品牌资源流程。

页面不使用分析追踪、远端字体或外部脚本。`assets/` 中图片可点击查看原图。
