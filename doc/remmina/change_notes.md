# Remmina 兼容性任务记录

## 2025-11-06：DesktopResize 强制同步
- **修改目的**：对齐 `rdp-cpp` 仓库 `c982bcce` 提交，避免 Remmina/FreeRDP 新版本在握手后因分辨率不一致而断开。
- **修改范围**：`src/session/grdc_rdp_session.c`、`.codex/plan/remmina-compatibility.md`、`doc/architecture.md`、`doc/changelog.md`。
- **修改内容**：
  1. 在 `GrdcRdpSession` 激活阶段调用新的 `grdc_rdp_session_enforce_peer_desktop_size`，根据运行时编码参数重写 `FreeRDP_DesktopWidth/Height` 并触发一次 `DesktopResize`。
  2. 新增任务计划文件与架构说明文档，固化 Remmina 兼容性修复背景。
  3. 建立变更记录文档，记录本次改动的目的、范围与影响。
- **项目影响**：客户端在激活阶段会被提示强制使用服务器编码尺寸，Remmina 将不再尝试缩放导致 `Invalid surface bits`，改动仅作用于连接初始化阶段，不影响后续帧传输逻辑。
