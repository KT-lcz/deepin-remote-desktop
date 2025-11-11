# 变更记录

## 2025-11-07：项目更名为 deepin-remote-desktop
- **目的**：将 GLib 重构版本统一纳入 Deepin Remote Desktop 品牌，避免 “grdc” 历史命名造成混淆。
- **范围**：`src/` 全量类型/函数/宏/文件、Meson 目标与可执行名、`doc/*.md`、`README.md`、`AGENTS.md` 及相关计划文档。
- **主要改动**：
  1. 所有 `grdc` 前缀统一替换为 `drd`（Deepin Remote Desktop），包含 `DrdRdpSession`、`DRD_LOG_*` 等命名。
  2. 静态库重命名为 `drd-media` / `drd-core`，可执行文件更名为 `deepin-remote-desktop`，Meson 工程名同步更新。
  3. 文档、计划、README 与开发指南更新说明新品牌，并替换运行命令（`./build/src/deepin-remote-desktop`）。
- **影响**：构建与启动命令发生变化；外部引用 `grdc` 的脚本需切换到新名称；上游快照中提及的 `grdctl` 不受影响。

## 2025-11-07：分辨率协商保护
- **目的**：阻止不支持 `DesktopResize` 的客户端在请求自定义分辨率时触发无限断连，避免日志刷屏并帮助用户定位问题。
- **范围**：`session/grdc_rdp_session.c`、`transport/grdc_rdp_listener.c`、`doc/architecture.md`、`.codex/plan/client-resolution-alignment.md`。
- **主要改动**：
  1. `grdc_rdp_session_enforce_peer_desktop_size()` 读取客户端 Capability，若未声明 `DesktopResize` 且分辨率与服务器要求不一致则拒绝激活并断开连接，同时补充统一的几何日志。
  2. `grdc_rdp_session_activate()` 只有在强制写回成功后才标记为 activated，失败时进入 `desktop-resize-blocked` 状态并给出原因。
  3. 监听器不再强制把 `FreeRDP_DesktopResize` 写为 TRUE，确保 Capability 值真实反映客户端支持度；架构文档同步描述新的保护流程。
  4. 新增 `Capabilities` 回调钩子，在握手阶段即检测 `DesktopResize` 能力，未满足立即拒绝连接并更新会话状态。
- **影响**：不兼容动态分辨率的客户端会在激活前被拒绝并提示原因，避免反复重连；符合规范的客户端不受影响，仍会被强制同步到服务器实际桌面尺寸。

## 2025-11-07：接入 NLA 安全协议
- **目的**：对齐 GNOME Remote Desktop 的 CredSSP 流程，让 `glib-rewrite` 通过 NLA 完成身份验证并阻止 TLS/RDP 降级。
- **范围**：`core/grdc_application.c`、`core/grdc_config.*`、`transport/grdc_rdp_listener.*`、`security/grdc_tls_credentials.c`、`security/grdc_nla_sam.*`、`config/default.ini`、`doc/architecture.md`、`.codex/plan/实现NLA安全协议.md`。
- **主要改动**：
  1. CLI/INI 新增 `--nla-username/--nla-password` 与 `[auth]` 配置，配置合并阶段校验凭据缺失即报错。
  2. 新增 `grdc_nla_sam` 模块生成临时 SAM 文件，监听器为每个 peer 注入 `FreeRDP_NtlmSamFile`，并在 PostConnect/析构时删除文件。
  3. 监听器切换到 `NlaSecurity=TRUE`、`TlsSecurity=FALSE`，TLS 模块仅负责证书注入；文档补充安全链路 mermaid 图与配置说明。
  4. 后续优化：在应用初始化时调用 `winpr_InitializeSSL()` 以启用 legacy 摘要算法，监听器仅缓存 NT 哈希并擦除明文密码，`grdc_nla_sam` 在写入后清零缓冲并校验 `NTOWFv1A()` 结果，避免 MIC 校验因哈希失败而触发。
  5. 所有 `g_message/g_warning/g_debug` 调用切换为 `GRDC_LOG_*` 宏，内部统一使用 `g_log_structured_standard` 自动附带 `__FILE__/__LINE__`，日志输出可直接定位源文件行号。
- **影响**：服务端现在要求显式提供 NLA 凭据；凭据泄露范围限定在内存+一次性 SAM 文件，客户端需支持 NLA（CredSSP）才能接入。

## 2025-11-08：接入 RDPGFX Progressive 管线
- **目的**：借鉴 GNOME Remote Desktop 的 Graphics Pipeline 实现，优先使用 Rdpgfx Progressive 推流，在兼容旧客户端的同时为后续 GPU/AVC 路径奠基。
- **范围**：`encoding/drd_rfx_encoder.*`、`encoding/drd_encoding_manager.*`、`core/drd_server_runtime.*`、`session/drd_rdp_session.*`、`session/drd_rdp_graphics_pipeline.*`（新增）、`transport/drd_rdp_listener.c`、`doc/architecture.md`、`doc/changelog.md`、`.codex/plan/rdpgfx-progressive.md`。
- **主要改动**：
  1. RFX 编码器支持 Progressive 输出（复用 FreeRDP progressive API），`DrdEncodingManager`/`DrdServerRuntime` 根据传输模式动态选择 RAW/RFX/Progressive，并在切换时刷新编码队列。
  2. 新增 `DrdRdpGraphicsPipeline`，对 Rdpgfx server context、Caps/Reset/Surface/Frame 流程进行最小封装，限制在 3 帧在途以避免 ACK 堵塞。
  3. `DrdRdpSession` 增加 Rdpgfx lifecycle：在虚拟通道建立后尝试打开 Graphics Pipeline，就绪时通知运行时输出 Progressive，若提交失败自动回退 SurfaceBits。
  4. `DrdRdpListener` 为 RFX 模式启用 Graphics Pipeline 能力，并将 WTS Virtual Channel Manager 句柄传递给会话，确保 Rdpgfx 与后续 DVC（剪贴板/音频）共享通道。
  5. 架构文档追加 Rdpgfx 模块描述与数据流说明，计划文档记录 Progressive 管线路线图。
- **影响**：支持 Graphics Pipeline 的客户端将以 Progressive（RFX Progressive）获取帧，带来更高压缩比；不支持或通道初始化失败时自动退回 SurfaceBits，无需额外配置。运行时切换编码模式会清空帧队列并强制关键帧，短暂突发在日志中可见。

## 2025-11-06：日志与注释审视
- **目的**：提升运行态可观察性并补齐核心线程流程的中文注释，方便后续排障。
- **范围**：`core/grdc_application.c`、`core/grdc_server_runtime.c`、`transport/grdc_rdp_listener.c`、`session/grdc_rdp_session.c`、`security/grdc_tls_credentials.c`、`doc/architecture.md`、`.codex/plan/logging-annotation.md`。
- **主要改动**：
  1. 应用层在加载配置文件与 TLS 凭据时输出详细日志，明确证书与私钥路径。
  2. 运行时停止、监听器启动以及 RDP 会话事件线程生命周期新增日志，关键路径可追踪。
  3. TLS 凭据加载与事件线程循环补充中文注释，阐明错误传播与等待模型。
  4. 架构文档同步描述新的日志行为，计划文档记录任务完成情况。
- **影响**：停服、监听循环与事件线程问题可快速定位；运行配置来源清晰，后续分析启动失败时更易还原环境。

## 2025-11-06：移除质量档位与 RLGR 固定策略
- **修改目的**：纠正 RLGR1/3 与画质档位的错误关联，避免质量配置误导用户；统一使用 RLGR3 并暂时下线“高/中/低”画质参数。
- **修改范围**：`core/grdc_encoding_options.h`、`core/grdc_config.*`、`core/grdc_application.c`、`encoding/grdc_encoding_manager.c`、`encoding/grdc_rfx_encoder.*`、`config/default.ini`、`doc/architecture.md`、`.codex/plan/rlgr-cleanup.md`。
- **修改内容**：
  1. 删除质量枚举、配置项与 CLI `--quality` 参数，`GrdcEncodingOptions` 不再包含质量字段，默认配置文件亦移除相关键。
  2. RFX 编码器固定切换为 RLGR3，去除 `quality_to_rlgr`、按档位调帧率的逻辑，编码管理器不再记录质量状态。
  3. 文档说明更新为“RFX 默认 RLGR3 + 帧差分”，计划文件同步记录任务背景。
- **项目影响**：现有配置无需再关注画质档位，所有 RFX 会话统一使用 RLGR3；若未来需要按带宽自适应，再在新迭代中补充独立机制。

## 2025-11-06：配置解析与输入注释优化
- **修改目的**：确保 `--config` 中的监听端口等配置不会被 CLI 默认值覆盖，同时让 X11 输入模块移除硬编码分辨率并补齐函数级注释，便于后续维护。
- **修改范围**：`core/grdc_application.c`、`core/grdc_config.c`、`input/grdc_x11_input.c`、`doc/architecture.md`、`.codex/plan/input-config-sync.md`。
- **修改内容**：
  1. CLI 端口默认值改为“未指定”语义，仅在用户显式传参时覆盖配置文件；补充应用、配置模块的函数注释。
  2. `grdc_x11_input` 在启动时读取真实桌面分辨率，若未收到编码流尺寸则使用该值作为初始映射，彻底移除 1920×1080 固定值，并为所有函数添加用途说明。
  3. 架构文档补充输入模块动态缩放行为，计划文件记录任务背景。
- **项目影响**：通过 `--config` 设置的端口、TLS 等字段将保持优先生效，输入注入日志更易追踪，指针映射在异常分辨率下也能准确缩放。

## 2025-11-06：Remmina 分辨率强制同步
- **修改目的**：复用 `rdp-cpp` `c982bcce` 提交思路，确保客户端在会话激活时被强制回写为服务器实际桌面尺寸，规避 Remmina 新版本报错 `Invalid surface bits command rectangle`。
- **修改范围**：`session/grdc_rdp_session.c`、`doc/architecture.md`、`.codex/plan/remmina-compatibility.md`。
- **修改内容**：
  1. 新增 `grdc_rdp_session_enforce_peer_desktop_size()`，在激活阶段重写 `FreeRDP_DesktopWidth/Height` 并调用 `DesktopResize`。
  2. 监听层计划文档与架构文档补充 Remmina 兼容性背景与同步策略。
  3. 计划清单记录任务背景与反馈节点便于跟踪。
- **项目影响**：会话激活阶段会产生一次额外的 `DesktopResize` 日志，客户端被强制保持服务器分辨率，防止握手后意外缩放导致 SurfaceBits 尺寸不一致。

## 2025-11-06：输入/视频管线排查（临时记录）
- 恢复 `grdc_rdp_session` 中的 SurfaceBits 推送逻辑，确保后续测试包含完整视频链路。
- 引入 FreeRDP 事件专用线程，采用 `WaitForMultipleObjects` 监听传输句柄，避免主循环阻塞导致的输入延迟；主循环仅负责编码与帧发送。
- `core/grdc_server_runtime` 增加编码线程与 `GAsyncQueue`，将屏幕捕获与编码解耦，保障帧发送与输入处理互不阻塞。
- `encoding/grdc_rfx_encoder` 强制首帧发送整帧（`force_keyframe`），搭配会话层 16ms 拉取超时，解决初始黑屏与首帧丢失问题。
- `session/grdc_rdp_session.c` 在空负载时仅发送 FrameMarker 心跳，避免差分帧缺失导致客户端保持黑屏。
- `core/grdc_server_runtime` 新增阻塞等待逻辑（`grdc_server_runtime_wait_encoded`），避免编码队列空读导致首帧或差分帧被过早消费。

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

## 2025-11-06：运行期精简与配置可观察性
- **目的**：去除冗余 TLS 初始化逻辑，提升配置可见性并精简运行时接口。
- **范围**：
  - `core/grdc_application` 合并配置后输出有效参数日志，统一 TLS 凭据创建流程。
  - `core/grdc_server_runtime` 精简 `pull_encoded_frame()` 签名，移除无用的 `max_payload` 参数。
  - `session/grdc_rdp_session` Raw SurfaceBits 推送增加中文注释并保持行分片实现。
- **主要改动**：
  - 监听器启动前验证证书路径存在，避免重复加载 TLS 资源。
  - 运行时拉帧 API 仅暴露超时与输出对象，接口语义更聚焦。
  - 会话层注释明确 Raw 帧分片原因，便于后续维护。
- **影响**：
  - 启动日志包含实际编码参数，部署排障更直观。
  - 运行时/会话接口简化，新调用者无需同步冗余参数。

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
