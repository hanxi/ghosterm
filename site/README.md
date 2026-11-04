# Ghosterm 落地页

发布地址：https://hanxi.cc/ghosterm/

纯静态 HTML / CSS，无依赖或构建步骤。GitHub Pages 只上传 `site/`，不会发布仓库其它目录。`site/**` 或 Pages 工作流变更推送到 main 后自动部署，也支持手动触发。仓库 Pages 的构建来源为 GitHub Actions。

本地预览（工程根目录执行）：

```bash
python3 -m http.server 8080 --directory site
```

打开 http://localhost:8080 。校验 HTML 的本地链接和资源、移动端布局与下载地址后提交。下载统一指向滚动 dev 资产，不在页面写死构建号、测试数量或包大小；平台能力以 `docs/reference/current-status.md` 和能力矩阵为准。

`assets/screenshots.json` 记录三张实际原生应用截图的来源及 SHA256。截图含隔离测试数据，按原图保存，没有使用 HTML 原型冒充产品截图；图注保留平台、历史界面和测试环境边界。图标来自 `assets/icons/ghosterm-mark.svg`，更新须通过仓库品牌资源流程。

页面不使用分析追踪、远端字体或外部脚本。`assets/` 中图片可点击查看原图。
