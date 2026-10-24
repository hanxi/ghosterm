# android — 宿主（Compose）

## 结构（规划）

```
android/
├── app/
│   ├── src/main/java/…/ui/         Compose 页面
│   ├── src/main/java/…/bridge/     JNI 绑定 → core/include/ghosterm.h
│   ├── src/main/cpp/               libghostty-vt + core 的 CMake 接线
│   └── src/main/jniLibs/           交叉编译产物（不入库）
```

## 要点

- 终端走 `terminal/android`（libghostty-vt + 自绘，理由见该目录 README）；
- 本地 shell 两档：内置 `android sh`（POSIX forkpty）；Termux 走
  `runtime-payload`（proot）；
- **Android 10+ W^X**：app 数据目录内不能直接 exec，proot 路线下由 proot 承担，
  proot 自身须先过这关（M5 首个 PoC）。

参考：[Chuchu](https://github.com/jossephus/chuchu)（MIT，Kotlin + Compose + libghostty-vt + libssh2 + Zig JNI）。
