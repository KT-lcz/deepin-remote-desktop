# Deepin Remote Desktop (drd) 架构概览

## 总体目标
- 以 C(GLib/GObject) 为核心，重构原 C++ RDP 服务端，保证单一职责、易扩展。
- 构建模块化层次：应用入口 → 运行时 → 采集/编码/输入 → 传输(RDP) → 对外接口。
- 在重构过程中逐步替换旧逻辑，避免新旧代码交叉依赖。

## 模块分层

### 1. 核心层
- `core/drd_application`：负责命令行解析、GLib 主循环、信号处理与监听器启动，并在 CLI/配置合并后记录生效参数及配置来源，确保 TLS 凭据只实例化一次（打包进 `libdrd-core.a`）。
- `core/drd_server_runtime`：聚合 Capture/Encoding/Input 子系统，`prepare_stream()` 配置三者后缓存 `DrdEncodingOptions`，`pull_encoded_frame()` 每次直接从 `DrdCaptureManager` 拉取最新帧并同步调用 `DrdEncodingManager` 编码，`set_transport()` 用于在 SurfaceBits 与 Rdpgfx 之间切换并强制关键帧；内部不再维护独立线程或 `GAsyncQueue`。
- `core/drd_config`：解析 INI/CLI 配置，集中管理绑定地址、TLS 证书、捕获尺寸等运行参数。
- `security/drd_tls_credentials`：加载并缓存 TLS 证书/私钥，供运行时向 FreeRDP Settings 注入。
- `security/drd_nla_sam`：基于用户名/密码生成临时 SAM 文件，写入 `FreeRDP_NtlmSamFile`，允许 CredSSP 在 NLA 期间读取 NT 哈希。

### 2. 采集层
- `capture/drd_capture_manager`：启动/停止屏幕捕获，维护帧队列。
- `capture/drd_x11_capture`：X11/XShm 抓屏线程，侦听 XDamage 并推送帧。
（以上与编码/输入/工具组成 `libdrd-media.a`，供核心库复用）

### 3. 编码层
- `encoding/drd_encoding_manager`：统一编码配置、调度；对外暴露帧编码接口，并在 Progressive 超出多片段限制时自动回退 RAW。
- `encoding/drd_raw_encoder`：原始帧编码器（BGRX → bottom-up），兼容旧客户端。
- `encoding/drd_rfx_encoder`：基于 RemoteFX 的压缩实现，支持帧差分与底图缓存；Progressive 路径固定使用 RLGR1（`rfx_context_set_mode(RLGR1)`）保持与 mstsc/gnome-remote-desktop 一致，SurfaceBits 仍以 RLGR3 为主。

### 4. 输入层
- `input/drd_input_dispatcher`：键鼠事件注入入口，管理 X11 注入后端与 FreeRDP 回调。
- `input/drd_x11_input`：基于 XTest 的实际注入实现，负责键盘、鼠标、滚轮事件，并在启动时读取真实桌面分辨率、根据编码流尺寸动态缩放坐标；同时在注入键盘事件时会把扩展按键的第 9 位（0xE0）剥离，只向 `freerdp_keyboard_get_x11_keycode_from_rdp_scancode()` 传递 8-bit scan code 与独立的 `extended` 标记，避免方向键等扩展扫描码超出 0–255 范围；若 FreeRDP 的旧映射返回 0（常见于 Alt/AltGr 等修饰键），则退回到 `XKeysymToKeycode()` 基于键值的查找，以确保修饰键必然可注入。

### 5. 传输层
- `transport/drd_rdp_listener`：FreeRDP 监听生命周期、Peer 接入、会话轮询，监听器在成功绑定后输出 tick-loop 日志便于诊断。
- `session/drd_rdp_session`：会话状态机，维护 peer/runtime 引用、虚拟通道、事件线程与 renderer 线程。`drd_rdp_session_render_thread()` 在激活后循环：等待 Rdpgfx 容量 → 调用 `drd_server_runtime_pull_encoded_frame()`（同步等待并编码）→ 优先提交 Progressive，失败则回退 SurfaceBits（Raw 帧按行分片避免超限 payload），并负责 transport 切换、关键帧请求与桌面大小校验。
- `session/drd_rdp_graphics_pipeline`：Rdpgfx server 适配器，负责与客户端交换 `CapsAdvertise/CapsConfirm`，在虚拟通道上执行 `ResetGraphics`/Surface 创建/帧提交；内部用 `needs_keyframe` 防止增量帧越级，并用 `capacity_cond`/`outstanding_frames` 控制 ACK 背压，当 Progressive 管线就绪时驱动运行时切换编码模式。

### 6. 通用工具
- `utils/drd_frame`：帧描述对象，封装像素数据/元信息。
- `utils/drd_frame_queue`：线程安全的单帧阻塞队列。
- `utils/drd_encoded_frame`：编码后帧的统一表示，携带 payload 与元数据。

## 数据流简述
1. 应用启动后创建 `DrdServerRuntime`，合并配置与 TLS 凭据，并在 `prepare_stream()` 中依次启动 capture/input/encoder。
2. `DrdCaptureManager` 启动 `DrdX11Capture`，将最新 `DrdFrame` 写入只保留一帧的 `DrdFrameQueue`；不存在额外编码线程或队列，capture 线程只负责刷新画面。
3. 会话激活后，`drd_rdp_session_render_thread()` 通过 `drd_server_runtime_pull_encoded_frame()` 同步等待帧并即时编码：若 Graphics Pipeline 就绪则走 Progressive（成功后将 runtime transport 设为 `DRD_FRAME_TRANSPORT_GRAPHICS_PIPELINE`），否则通过 `SurfaceBits` + `SurfaceFrameMarker` 推送，Raw 帧按行拆分以满足多片段上限。`drd_rdp_session_pump()` 仅在 renderer 尚未启动时退化为 SurfaceBits 发送。

## 安全链路（TLS + NLA）
- `config/default.ini` 及 CLI 新增 `[auth]`/`--nla-{username,password}`，服务进程启动时必须提供 NLA 凭据，才能生成 SAM 文件。
- `DrdRdpListener` 在 `drd_configure_peer_settings()` 阶段串联 TLS 证书与 SAM 文件：一方面走 `drd_tls_credentials_apply()` 注入 PEM，另一方面使用 `drd_nla_sam_file_new()` 生成一次性数据库路径并设置 `FreeRDP_NtlmSamFile`。
- 监听器在设置展示参数后强制 `NlaSecurity=TRUE`、`TlsSecurity=FALSE`、`RdpSecurity=FALSE`，从而锁定 CredSSP，不再回退纯 TLS/RDP。
- SAM 文件在 `PostConnect` 即 CredSSP 完成后立即删除，避免磁盘残留；失败路径由 peer context 析构兜底。

```mermaid
sequenceDiagram
    participant CFG as DrdConfig
    participant APP as DrdApplication
    participant LSN as DrdRdpListener
    participant SAM as DrdNlaSamFile
    participant FRDP as FreeRDP Server
    CFG->>APP: TLS paths + NLA username/password
    APP->>LSN: Pass bind/port/runtime + credentials
    LSN->>SAM: g_mkstemp + NTOWFv1A -> temp SAM
    SAM-->>LSN: expose file path
    LSN->>FRDP: RdpServerCertificate, NtlmSamFile, NlaSecurity=TRUE
    FRDP-->>SAM: Read NT hash during CredSSP
    LSN->>SAM: Delete file after PostConnect/cleanup
```

## 设计原则落实
- **SOLID**：各模块限定单一职责；监听器依赖抽象的 runtime；待迁移的编码/输入将通过接口剥离具体实现。
- **KISS/YAGNI**：阶段性仅实现最小可运行路径（监听 + 采集），编码/输入按需延伸。
- **DRY**：帧结构、队列作为共享组件供捕获/编码/传输复用。

## RDP 分辨率同步策略
- 运行时负责维护最新的 `DrdEncodingOptions`，监听器在 `freerdp_peer` 初始化时根据该选项写入 `FreeRDP_DesktopWidth/Height`、RemoteFX 能力并禁用 DisplayControl/MonitorLayout，以静态分辨率保障为主。
- 监听器挂接 `client->Capabilities` 回调，若客户端在 Capability 交换中未声明 `DesktopResize`，立即拒绝连接并提示客户端当前分辨率，避免进入激活态后才发现冲突。
- 会话在 `Activate` 阶段调用 `drd_rdp_session_enforce_peer_desktop_size()`，再次读取编码宽高并回写到 `rdpSettings`，若发现客户端偏离则立即触发一次 `DesktopResize`。
- 若客户端未在 Capability 阶段声明 `DesktopResize` 支持且仍坚持非服务器分辨率，会话直接拒绝激活并记录告警，防止无限重连；只有在 FreeRDP 回调链提供 `DesktopResize` 时才执行强制回写。
- 通过上述多级同步，Remmina/FreeRDP 新版本即便尝试窗口缩放也会被强制回调至服务器实际桌面尺寸，帧推流始终匹配编码几何，避免 `Invalid surface bits`。



## glib-rewrite RDPGFX 初始化与锁策略
- `drd_rdp_session_vcm_thread` 轮询 FreeRDP 事件，监听 `DRDYNVC_STATE_READY` 后唤起 `drd_rdp_graphics_pipeline_maybe_init()` 完成 Rdpgfx 管线初始化。
- 初始化流程必须在调用 `rdpgfx_context->Open()` 前释放 `DrdRdpGraphicsPipeline::lock`，因为 FreeRDP 会在 `Open()` 过程中同步触发 `ChannelIdAssigned`/`CapsAdvertise` 回调，而这些回调同样会再次进入管线对象并尝试获取同一把锁。
- 只有在 `caps_confirmed` 置位后，才能调用 `ResetGraphics`/`CreateSurface`/`MapSurfaceToOutput` 来准备 RFX Progressive Surface；否则应继续等待 VCM 回调驱动能力协商。

```mermaid
sequenceDiagram
    participant T as drd_rdp_session_vcm_thread
    participant P as DrdRdpGraphicsPipeline
    participant F as FreeRDP Rdpgfx

    T->>P: maybe_init()
    P->>P: lock()
    alt channel 尚未建立
        P-->>P: unlock()
        P->>F: rdpgfx_context->Open()
        F-->>P: ChannelIdAssigned/CapsAdvertise
        P->>P: lock()\nchannel_opened = TRUE\ncaps_confirmed = TRUE
    end
    P->>P: ResetGraphics/CreateSurface/MapSurfaceToOutput
    P-->>T: ready 状态（可切换至 GFX 传输）
```

## Progressive RFX 帧封装
- `DrdRfxEncoder` 新增 `drd_rfx_encoder_write_progressive_message()`（`glib-rewrite/src/encoding/drd_rfx_encoder.c:218-379`），按 [MS-RDPEGFX] 规范依次写入 `SYNC`、`CONTEXT`、`FRAME_BEGIN/REGION/TILE/FRAME_END`，确保 Windows 客户端能正确解码。
- FreeRDP 提供的 `progressive_rfx_write_message_progressive_simple()` 仅生成精简流，缺少 `RFX_PROGRESSIVE_CONTEXT`/`REGION` 元数据，导致客户端侧色彩错位；因此编码器改为显式构建 Header 并追踪 `progressive_header_sent` 状态（`glib-rewrite/src/encoding/drd_rfx_encoder.c:404-414`）。
- 为了在强制关键帧或重新配置后刷新上下文，每次触发 `drd_rfx_encoder_force_keyframe()` 都会复位 Header 标记，保证下一帧重新携带同步块（`glib-rewrite/src/encoding/drd_rfx_encoder.c:623-628`）。
- Progressive 帧提交路径增加了 `needs_keyframe` 守卫：`drd_rdp_graphics_pipeline_submit_frame()` 会拒绝在尚未发送关键帧时提交增量帧，并通过新的 `DRD_RDP_GRAPHICS_PIPELINE_ERROR_NEEDS_KEYFRAME` 错误提示会话调用 `drd_server_runtime_request_keyframe()`，防止客户端收到缺失 Progressive Header 的数据（`glib-rewrite/src/session/drd_rdp_graphics_pipeline.c`、`drd_rdp_session.c`）。关键帧标记在会话层读取后随参数传入，避免编码线程重用同一 `DrdEncodedFrame` 对象时造成竞态。
- Progressive 路径默认切换到 `RLGR1`，保持与 `gnome-remote-desktop` 及 Windows mstsc 的兼容性；SurfaceBits 仍可沿用 `RLGR3` 以追求更高压缩率。若需要调试全量帧，可在 `[encoding] enable_diff=false` 或调用 `drd_server_runtime_request_keyframe()`。

## 单线程编码与发送
- `DrdServerRuntime` 不再维护独立的编码线程 / `encoded_queue`。`drd_server_runtime_pull_encoded_frame()` 会直接从 `DrdCaptureManager` 取出最新 `DrdFrame`，立刻调用 `DrdEncodingManager` 同步编码并将 `DrdEncodedFrame` 返回给会话线程（`glib-rewrite/src/core/drd_server_runtime.c:146-191`）。
- 每个会话在 `Activate` 后都会启动专用 renderer 线程（`drd_rdp_session_render_thread()`，`glib-rewrite/src/session/drd_rdp_session.c:800+`）。该线程串行执行 “等待 capture 帧 → 编码 → Rdpgfx 提交 / SurfaceBits 回退”，与 GNOME 的 graphics 线程一致，同时释放 GLib 主循环。
- `drd_rdp_session_pump()` 仅在 renderer 尚未启动时工作（例如激活前），避免重复发送；帧序号仍由 session 维护，renderer 线程结束后自动释放引用。
- 传输模式切换（SurfaceBits ↔ Progressive）仅更新 `transport_mode` 并强制下一帧关键帧，不再需要清空编码队列；renderer 会等待下一帧并立即产出关键帧，方便调试完整画面。

## FrameAcknowledge 与 Rdpgfx 背压
- `DrdRdpGraphicsPipeline` 维护 `outstanding_frames`/`max_outstanding_frames` 与 `capacity_cond`；renderer 线程在调用 `drd_rdp_graphics_pipeline_wait_for_capacity()` 时会在 `capacity_cond` 上阻塞，直至 `FrameAcknowledge` 或提交失败唤醒，确保“客户端确认一帧→服务器再发送下一帧”。
- 客户端发送的 `RDPGFX_FRAME_ACKNOWLEDGE_PDU`（`frameId`、`totalFramesDecoded`、`queueDepth`）在 `drd_rdpgfx_frame_ack()` 中被消费：当前实现仅将 `outstanding_frames` 减 1 并广播 `capacity_cond`，尚未读取 `queueDepth` 进一步调节速率，背压主要依靠 `max_outstanding_frames`。
- 如果在超时时间内一直得不到 ACK，会话会调用 `drd_rdp_session_disable_graphics_pipeline()` 回退 SurfaceBits，并通过 `drd_server_runtime_request_keyframe()` 在恢复时强制全量帧，保证客户端状态重新对齐。

## Renderer & Capture 线程协作
- **捕获线程**：`drd_x11_capture_thread()` 监听 XDamage 事件，更新 `DrdFrameQueue` 中的最新帧；队列只存一帧，保证 renderer 线程消费时永远拿到最新画面。
- **Renderer 线程**：`drd_rdp_session_render_thread()` 在 `render_running` 标志下循环：等待 Rdpgfx 容量 → 调用 `drd_server_runtime_pull_encoded_frame()`（同步等待并编码）→ 优先提交 Progressive，失败则退回 SurfaceBits；过程中持续维护 `frame_sequence`、关键帧状态和 `needs_keyframe` 标志，且无需额外 `DrdRdpRenderer` 模块。
- **生命周期**：renderer 线程在会话 `Activate` 时启动，`drd_rdp_session_stop_event_thread()`/`drd_rdp_session_disable_graphics_pipeline()` 会在断开或切换时停止线程并重置状态，确保 capture/renderer 不会引用失效的 `freerdp_peer`。

```mermaid
flowchart LR
  subgraph Capture Thread
    X11[XDamage\nXShmGetImage] --> Q[DrdFrameQueue\n(仅缓存最新帧)]
  end
  subgraph Renderer Thread
    Q --> |wait_frame| ENCODE[DrdEncodingManager\n(RLGR1 Progressive)]
    ENCODE --> |needs_keyframe?| PIPE[DrdRdpGraphicsPipeline\nSurfaceFrameCommand]
    PIPE --> |FrameAck| COND[capacity_cond broadcast]
    ENCODE --> |fallback| SURF[SurfaceBits]
  end
  subgraph RDP Client
    PIPE --> CLIENT
    CLIENT --> |RDPGFX_FRAME_ACKNOWLEDGE| COND
  end
```

## Rdpgfx 背压与关键帧修复（2025-11-12）
- `DrdRdpGraphicsPipeline` 新增 `capacity_cond` 条件变量，`FrameAcknowledge` 以及提交失败都会唤醒等待者，`drd_rdp_graphics_pipeline_wait_for_capacity()` 允许在握有同一把锁的情况下等待 “未确认帧 `< max_outstanding_frames`” 的判定（`glib-rewrite/src/session/drd_rdp_graphics_pipeline.c:24-116`、`:264-333`、`:389-452`）。
- 会话渲染逻辑直接内嵌在 `drd_rdp_session_render_thread()`（`glib-rewrite/src/session/drd_rdp_session.c`）中：线程串行调用 `drd_rdp_graphics_pipeline_submit_frame()`，失败时立即请求关键帧或降级，无需单独 `DrdRdpRenderer` 模块。
- `drd_rdp_session_try_submit_graphics()` 在提交 Progressive 帧前若检测到拥塞，会调用 `drd_rdp_graphics_pipeline_wait_for_capacity()` 并一直阻塞直到 ACK/管线重置释放槽位；若等待返回仍无法提交，则禁用 Rdpgfx 回退 SurfaceBits，同时强制关键帧，避免客户端长时间灰屏。
- 通过 renderer + 条件变量，rdpgfx 在正常情况下不会直接丢帧；当客户端未发送 ACK 时，系统会自动降级并刷新关键帧，确保画面尽快恢复。

```mermaid
sequenceDiagram
    participant Pump as drd_rdp_session_render_thread
    participant Renderer as SessionRenderer
    participant Pipeline as DrdRdpGraphicsPipeline
    participant Client as RDP Client

    Pump->>Renderer: enqueue progressive frame
    Renderer->>Pipeline: wait_for_capacity()
    alt ACK arrives
        Client-->>Pipeline: FrameAcknowledge
        Pipeline-->>Renderer: capacity_cond signal
        Renderer->>Pipeline: submit_frame()
    else Failure
        Renderer->>Pump: notify error/disable pipeline
        Pump->>Pipeline: disable & fallback
    end
    Pipeline->>Client: Start/Surface/EndFrame
    Client-->>Pipeline: FrameAcknowledge
    Pipeline-->>Renderer: capacity_cond signal（允许下一帧）
```

## 会话生命周期与重连（2025-11-14）
- `DrdRdpPeerContext` 现在持有 `DrdRdpListener *listener`，当 FreeRDP 触发 `Disconnect` 或上下文析构时能够回调监听器，保持单一职责：会话对象仍聚焦图像/输入流，而连接资源释放由监听器集中处理。
- `drd_rdp_listener_session_closed()` 负责从 `sessions` 数组中移除断线的 `DrdRdpSession`，只有当真正找到了匹配项才输出日志并减少引用计数，避免重复移除造成的未定义行为。
- `drd_peer_disconnected()` 与 `drd_peer_context_free()` 都会调用该方法，从而覆盖正常断线与握手失败两条路径，确保 `sessions->len` 回落到 0，新的客户端即可重新接入，不再出现 “session already active”。
- `DrdRdpSession` 在 VCM 线程退出或 `drd_rdp_session_stop_event_thread()` 完成时会触发一次性关闭回调，由监听器在主线程内同步移除 `sessions` 元素，即使 FreeRDP 未调用 `client->Disconnect` 也不会遗留僵尸会话。
- 由于监听器自己的生命周期仍由 `DrdApplication` 管控，不需要在移除会话时立即 `stop()` runtime，避免下一次连接时缺少捕获/编码链路。

```mermaid
stateDiagram-v2
    [*] --> Idle: Listener started
    Idle --> Active: PeerAccepted\n(g_ptr_array_add)
    Active --> Closing: FreeRDP Disconnect\n或 ContextFree
    Closing --> Idle: drd_rdp_listener_session_closed\n(g_ptr_array_remove_fast)
```

## 待优化方向
- **Rdpgfx 失联超时**：目前 `drd_rdp_session_try_submit_graphics()` 传入 `-1` 调用 `drd_rdp_graphics_pipeline_wait_for_capacity()`，若客户端停止发送 ACK，renderer 线程会无限阻塞，只有连接关闭或手动禁用 Rdpgfx 才能恢复。需要增加超时/心跳和自动降级逻辑，避免服务端被拖死。
- **Unicode 注入缺失**：`drd_x11_input_inject_unicode()` 仍是空实现（直接返回 TRUE），Remmina/Windows 发送的 `Unicode` 事件被丢弃。应补全 UTF-16 → X11 Keysym 映射并复用 `XTestFakeKeyEvent`，否则国际化文本只能依赖组合键。
- **多显示器拓扑**：`drd_rdp_graphics_pipeline_reset_locked()` 目前发送 `monitorCount=0`，客户端始终认为只有单显示器，无法感知真实布局。需要把 Xinerama/Monitor 信息注入 `ResetGraphics`/`MapSurfaceToOutput`，并在 DisplayControl 关闭的前提下仍提供正确的输出坐标。
***

## 遗留问题
1. 快捷键和其他功能键无法使用
2. 重连不成功
3. 偶发断连的情况
