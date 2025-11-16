# 任务：视频帧推送控制分析

## 背景
- 用户需要明确远程桌面服务端推送视频帧的控制权归属，以便判断是客户端还是服务端驱动输出。
- 代码基于 FreeRDP/GLib，核心逻辑位于 `session/drd_rdp_session` 与 `session/drd_rdp_graphics_pipeline`，并依赖 `core/drd_server_runtime`。

## 计划
- [x] 步骤1：梳理 `drd_rdp_session_render_thread()` 与 `drd_server_runtime_pull_encoded_frame()` 的帧生成/发送流程，确认是否由服务端主动拉帧并推送。
- [x] 步骤2：分析 `drd_rdp_graphics_pipeline` 中的 `FrameAcknowledge` 背压机制，评估客户端能否通过 ACK 控制推送节奏。
- [x] 步骤3：结合两条路径（SurfaceBits 与 Rdpgfx），整理成结论，说明控制权的划分，并列出潜在优化/关注点；请用户确认是否需要更深入的 trace。

## 遗留/风险
- 已输出结论与优化建议，等待用户反馈是否还需追踪 FreeRDP 更底层的回调。
