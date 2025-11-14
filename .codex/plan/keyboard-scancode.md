# 键盘扫描码超界故障

## 上下文
- 客户端发送方向键（扫描码 0xE0 0x48/0x50/0x4B/0x4D 等）时，FreeRDP 日志报错 `ScanCode 336 exceeds allowed value range [0,256]`，服务端无法响应输入。
- 需排查 `DrdInputDispatcher` 与 X11 注入链路，确认服务器为何传入超界扫描码，或是否缺少扩展按键处理。

## 计划
1. [x] 阅读 FreeRDP 输入回调路径（`drd_rdp_listener.c` → `drd_rdp_peer_keyboard_event` → `DrdInputDispatcher`），确定传入的 `flags`/`code` 含义，以及当前对扩展键的处理策略。
2. [x] 修复扫描码映射：若 `code` 含 `KBDFLAGS_EXTENDED`，应将高位（0xE0）与 8-bit scancode 分离再传递，或直接走 Unicode/keysym 映射；同时确保超界值不会传入 FreeRDP X11 转换函数。
3. [x] 运行 `meson compile -C build`（必要时补充单测）验证无回归，并指导用户复测方向键。
4. [x] 更新 `doc/architecture.md`/`doc/changelog.md` 或补充输入模块文档，记录扩展键处理方式，并同步计划状态。

## 新问题：Alt 键无响应
5. [x] 复现 Alt 键（左/右、组合键）无效的现象，核对 `flags`/scancode 与当前 `MAKE_RDP_SCANCODE` 拆分方式的关系。
6. [x] 针对 Alt 键特殊要求（右 Alt = AltGr）补充映射或改用 Unicode 路径，确保 `freerdp_keyboard_get_x11_keycode_from_rdp_scancode()` 获得正确的 extended/scan code 组合。
7. [x] 重新构建验证，并请用户复测 Alt 功能。
8. [x] 如涉及逻辑调整，更新文档/计划进度。
