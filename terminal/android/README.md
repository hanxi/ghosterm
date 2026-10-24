# terminal/android — libghostty-vt + 自绘

理由：full libghostty 的 OpenGL 渲染器要求 **OpenGL 4.3 core**，Android 只有
**OpenGL ES**，无可用后端（见 `docs/architecture/overview.md`）。

## 组成

- `libghostty-vt.a`（vendored）——VT 解析 + 渲染状态
- 自绘层：GLES（`SurfaceView`）或 Canvas，按 `ghostty_render_state_*` 画格子
- 输入：`ghostty_key_encoder_*` / `ghostty_mouse_encoder_*` 编码按键与鼠标

## 参考实现（MIT，可直接对照）

- [Chuchu](https://github.com/jossephus/chuchu)：Kotlin + Compose + libghostty-vt + libssh2，
  Zig 写 JNI 桥，已能日常使用
- [expo-libghostty](https://github.com/arcboxlabs/expo-libghostty)：libghostty-vt + JNI + Skia 位图渲染

## 待办（M5）

- [ ] vendored `libghostty-vt` 交叉编译（NDK/zig）
- [ ] GLES 自绘 renderer（脏行更新）
- [ ] IME / 组合键 / 软键盘辅助条（Chuchu 有完整实现可对照）
