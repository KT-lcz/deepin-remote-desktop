# 变更记录

## 2025-11-06：输入/视频管线排查（临时记录）
- 恢复 `grdc_rdp_session` 中的 SurfaceBits 推送逻辑，确保后续测试包含完整视频链路。
- 引入 FreeRDP 事件专用线程，采用 `WaitForMultipleObjects` 监听传输句柄，避免主循环阻塞导致的输入延迟；主循环仅负责编码与帧发送。
- `core/grdc_server_runtime` 增加编码线程与 `GAsyncQueue`，将屏幕捕获与编码解耦，保障帧发送与输入处理互不阻塞。
- `encoding/grdc_rfx_encoder` 强制首帧发送整帧（`force_keyframe`），搭配会话层 16ms 拉取超时，解决初始黑屏与首帧丢失问题。

## 2025-11-06：FreeRDP 参数同步与 RFX 传输修整
- **目的**：
  - 对齐原项目的 FreeRDP 会话配置，避免 SurfaceBits/RemoteFX 在协商阶段出现尺寸或编码能力不匹配。
  - 提供运行时编码参数查询接口，便于监听器按实际采集分辨率配置客户端。
  - 在 RDP 会话层补齐 SurfaceFrameMarker 生命周期管理与负载上限检测，提前拦截超规格帧。
- **范围**：
  - `core/grdc_server_runtime.[ch]` 新增编码参数查询 API，供传输层读取。
  - `transport/grdc_rdp_listener.c` 同步 RemoteFX/输入/图形管线相关 FreeRDP 设置，并根据配置写入桌面分辨率、TLS、安全标志。
  - `session/grdc_rdp_session.c` 精简 SurfaceBits 分支：按帧编码类型分流、校验协商负载上限、避免重复变量与 FrameMarker 失配；新增激活状态守卫，避免在客户端握手完成前发送帧。
  - `encoding/grdc_encoding_manager.c` 允许依据协商的 `MultifragMaxRequestSize` 约束 RFX 帧，必要时回退 Raw 编码；`core/grdc_server_runtime.[ch]`、`session/grdc_rdp_session.[ch]` 透传该参数。
  - 监听器将 `FreeRDP_MultifragMaxRequestSize` 保持为 0（默认无限制），避免大尺寸帧被强制切回 Raw 编码。
- **主要改动**：
  - 运行时记录的编码选项向外暴露，监听器按需写入 FreeRDP settings（DesktopWidth/Height、RemoteFxCodec、FastPathOutput 等）并维持禁止 H264 的约束。
  - 会话层在发送 RFX 帧时先检查协商的 `MultifragMaxRequestSize`，若超限则回退 Raw 路径，同时保证 FrameAction Begin → End 的对称性。
  - Raw 分支保留逐块下发逻辑，并复用协商出的最大负载参数，避免硬编码。
  - `encoding/grdc_rfx_encoder` 保持自顶向下的像素顺序，与 X11 捕获输出一致，避免远端画面倒置。
  - `input/grdc_x11_input` 根据捕获分辨率与物理桌面尺寸计算指针缩放，确保客户端坐标映射准确，消除远端拖动缓慢的体验。
  - `utils/grdc_frame_queue` 在 `timeout_us == 0` 时改为非阻塞返回，避免会话循环在无帧更新时挂起，从而保障输入事件及时处理。
- **影响**：
  - 新增接口需要在后续模块调用时注意判空，监听器若获取失败会主动拒绝连接。
  - 远端客户端现在与采集端分辨率保持一致，后续在引入桌面重设逻辑时可直接复用该流程。
  - 每帧根据客户端协商值动态选择 RFX/Raw，后续仍需补充多片段编码与差分策略以降低带宽占用。

## 2025-XX-XX：编码链路打通（当前迭代）
- **目的**：
  - 建立采集→编码的最小可用数据通路，为 RDP SurfaceBits 发送做准备。
  - 提供统一的编码输出结构，方便后续接入多种编码器。
- **范围**：
  - 新增 `utils/grdc_encoded_frame*`、`encoding/grdc_raw_encoder`，扩展 `grdc_encoding_manager` 与 `grdc_server_runtime`。
  - `grdc_capture_manager` 增加帧等待接口，`grdc_rdp_session` 暴露编码帧提取与 SurfaceBits 推送逻辑。
  - 构建 `input/grdc_x11_input` 与增强版 `grdc_input_dispatcher`，完成键鼠注入路径。
  - 新增 `security/grdc_tls_credentials`、命令行 `--cert/--key`，以及监听侧的 TLS 强制配置与单会话策略。
- **主要改动**：
  - 实现 Raw 编码器（BGRX → bottom-up）并整合到编码管理器。
  - `GrdcServerRuntime` 支持按需拉取编码帧，自动从采集队列取帧并编码。
  - 会话层提供 `grdc_rdp_session_pull_encoded_frame()` 与 SurfaceBits 推送逻辑；输入侧接入 FreeRDP Keyboard/Pointer 回调实现端到端闭环。
  - 传输层加载 TLS 证书/私钥并拒绝多会话并发，保障连接安全性与资源独占。
- **2025-XX-XX：配置体系与 TLS 强制启用**
  - **目的**：提供可配置的启动参数加载方式，并在 CLI 层面强制 TLS 证书/私钥输入。
  - **范围**：新增 `core/grdc_config`、命令行 `--config/--cert/--key`；`GrdcApplication` 与运行时之间传递配置对象。
  - **主要改动**：
    - 支持从 INI 文件加载 server/tls/capture 段落，并允许 CLI 覆盖关键字段。
    - 启动时根据配置实例化 `GrdcTlsCredentials` 并写入 FreeRDP settings。
    - 日志输出绑定地址、端口与证书路径，提升可观察性。
  - **影响**：运行程序需显式提供证书/私钥（或配置文件），为后续 TLS/认证扩展奠定基础。
- **影响**：
  - 代码现在可在 GLib 线程模型内完成采集与编码的完整闭环，旧 C++ 编码逻辑可逐步淘汰。
  - 后续仅需在传输层拼装 SurfaceBits/Stream 即可实现端到端画面输出。
  - 项目内置默认自签名证书（`config/default.ini` + `certs/server.*`），方便本地测试；后续将补充自动化证书生成工具。

## 2025-XX-XX：采集模块重构（当前迭代）
- **目的**：
  - 替换旧 C++ X11 抓屏实现，为后续纯 C/GLib 架构铺路。
  - 引入统一的帧结构与队列，打通采集与编码的标准接口。
- **范围**：
  - 新增 `utils/grdc_frame*`、`capture/grdc_x11_capture` 等模块。
  - 更新 `grdc_capture_manager` 与 `grdc_server_runtime` 使其依赖新组件。
  - Meson 引入 X11/XDamage 依赖。
- **主要改动**：
  - 实现 XShm + XDamage 抓屏线程，封装为 `GrdcX11Capture`。
  - 定义 `GrdcFrame`（像素缓冲 + 元数据）与 `GrdcFrameQueue`（阻塞队列）。
  - 采集管理器、运行时更新，以 `GrdcFrameQueue` 为共享通道。
- **影响**：
  - 编译新增依赖（X11/Xext/Xdamage/Xfixes）。
  - 采集路径完全脱离旧 C++ 代码，其余模块可逐步迁移。
  - 暂未接入编码/传输的数据通路，需要后续迭代串联。

## 2025-XX-XX：基础骨架搭建
- **目的**：建立 C/GLib 项目结构、最小 FreeRDP 监听链路。
- **范围**：`core/` 应用入口、`transport/` 监听器、`session/` 会话管理、Meson 构建脚本。
- **主要改动**：
  - `GrdcApplication` 管理主循环与信号处理。
  - `GrdcRdpListener` 与 `GrdcRdpSession` 构建最小连接链路并修复初始化崩溃。
  - 引入 `GrdcServerRuntime` 骨架。
- **影响**：提供可编译/运行的基础框架，便于逐步接入功能模块。
