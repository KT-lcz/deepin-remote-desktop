# 变更记录

## 2025-12-10：编码帧 payload 封装
- **目的**：消除外部对 `DrdEncodedFrame` 内部 payload 指针的直接操作，集中管理编码结果写入，降低遗漏长度同步的风险。
- **范围**：`src/utils/drd_encoded_frame.*`、`src/encoding/drd_raw_encoder.c`、`src/encoding/drd_rfx_encoder.c`、`doc/architecture.md`、`.codex/plan/drd_encoded_frame_payload_encapsulation.md`。
- **主要改动**：
  1. 新增 `drd_encoded_frame_set_payload`/`drd_encoded_frame_fill_payload` 封装写入，writer 返回 `gboolean`，失败会回滚长度，支持直接复制或回调填充。
  2. RFX 编码改用 `set_payload` 写入编码流，失败路径补充错误返回。
  3. RAW 编码通过 `fill_payload` 底朝上逐行写入，回调校验 size 并返回状态，避免暴露内部指针。
  4. 架构文档补充工具层 API 与类图，计划文件更新进度。
- **影响**：写 payload 时不再返回内部指针，写入失败可回滚，接口边界更清晰；编码输出行为保持不变。

## 2025-12-09：capture/core/security/system 注释对齐 encoding 规范
- **目的**：为核心运行时、采集、安全与 system 守护模块补充与 encoding 模块一致的中文注释格式，明确功能、逻辑、参数及外部库调用。
- **范围**：`src/capture/*.c`、`src/core/*.c`、`src/security/*.c`、`src/system/*.c`、`.codex/plan/comment-capture-core-security-system.md`。
- **主要改动**：
  1. `DrdCaptureManager`、`DrdX11Capture` 补充捕获启动/节流/唤醒管道等流程注释，标注 XDamage/XShm 与 GLib 线程/管道接口。
  2. `drd_application`、`drd_config`、`drd_server_runtime` 增加配置解析、TLS/NLA 校验、流启动与传输模式切换的中文说明，覆盖 g_option/GDBus/GLib 原子操作调用点。
  3. `drd_local_session`、`drd_nla_sam`、`drd_tls_credentials` 记录 PAM/WinPR/FreeRDP 的交互接口，说明凭据加载、哈希生成与 PEM 注入 FreeRDP settings 的流程。
  4. `drd_handover_daemon`、`drd_system_daemon` 描述 handover/system DBus 信号、routing token 分配、连接接管与 LightDM 远程 display 创建路径，明确 g_bus_own_name、DBus skeleton 导出与连接元数据的使用。
- **影响**：提升跨模块代码可读性与外部库交互透明度，无行为改动，为后续维护和排障提供统一注释格式。

## 2025-12-09：input/utils/main 注释补齐
- **目的**：完成剩余 input、utils 及入口模块的中文注释对齐，覆盖功能、逻辑、参数与外部库调用说明。
- **范围**：`src/input/*.c`、`src/utils/*.c`、`src/main.c`、`.codex/plan/comment-utils-input-main.md`。
- **主要改动**：
  1. `DrdInputDispatcher`、`DrdX11Input` 增补启动/注入/坐标缩放/键码缓存等流程注释，标注 XTest、FreeRDP 键盘映射接口。
  2. `drd_frame*`、`drd_encoded_frame`、`drd_frame_queue` 记录缓冲配置、环形队列推送/等待/丢帧统计逻辑，`drd_log` 说明自定义 GLib writer。
  3. `drd_tls_credentials` 清理 NTLM hash 栈残留，入口 `main` 标注 WinPR/WTS 初始化流程。
- **影响**：全仓库函数注释格式统一，便于快速理解输入注入、帧缓存与日志初始化路径，无功能行为变更。

## 2025-12-08：RDP 监听器失败清理收敛
- **目的**：统一 `DrdRdpListener` 的失败清理逻辑，减少连接关闭与 peer 释放的重复分支，降低遗漏风险。
- **范围**：`src/transport/drd_rdp_listener.c`、`doc/architecture.md`、`.codex/plan/rdp_listener_cleanup.md`。
- **主要改动**：
  1. 新增内部清理辅助函数，按需关闭 `GSocketConnection` 并回收 `freerdp_peer`，复用 keep-open 语义。
  2. `drd_rdp_listener_handle_connection()` 的失败与成功路径统一复用该清理函数，避免多处手动 `g_io_stream_close()`/`freerdp_peer_free()`。
  3. `drd_rdp_listener_close_connection()` 增加 `G_IS_SOCKET_CONNECTION` 防御检查，避免未来误传非连接对象导致 unref 崩溃。
- **影响**：连接失败分支的释放逻辑集中，后续调整只需修改单处，降低重复维护和遗漏关闭的概率，正常成功路径行为保持不变。

## 2025-12-05：编码/会话/传输模块注释完善
- **目的**：为编码、会话、传输模块的全部函数补充中文注释，便于后续维护和排查跨库调用细节。
- **范围**：`src/encoding/*.c`、`src/session/*.c`、`src/transport/*.c`、`.codex/plan/encoding-session-transport-comments.md`。
- **主要改动**：
  1. `DrdEncodingManager`、Raw/RFX 编码器补充功能、逻辑、参数与 FreeRDP/WinPR 交互说明，覆盖关键帧、回退策略及差分 tile 处理。
  2. `DrdRdpSession` 与 `DrdRdpGraphicsPipeline` 全量注释生命周期、线程、Rdpgfx/SurfaceBits 传输路径及关键帧/背压处理，并标注 FreeRDP/WinPR 事件与流接口。
  3. `DrdRdpListener`、路由令牌工具新增连接接入、NLA/TLS 认证、输入分发、RDSTLS 握手窥探等流程说明，明确依赖的 GLib/FreeRDP/WTS/WinPR API。
- **影响**：提升代码可读性与交接效率，无功能行为变更。

## 2025-12-05：X11 捕获帧率节流
- **目的**：当 `target_interval` 调低（如 24fps）时，damage 风暴不再把实际帧率推到显示刷新频率，降低带宽占用。
- **范围**：`src/capture/drd_x11_capture.c`、`doc/architecture.md`、`.codex/plan/x11-capture-throttle.md`。
- **主要改动**：
  1. `drd_x11_capture_thread()` 引入 `damage_pending` 累积 XDamage 事件，统一按 `target_interval` 触发抓帧，即使事件持续到达也会等待到期。
  2. 抓帧时间戳改为在实际复制像素前重新取 `g_get_monotonic_time()`，避免节流等待后时间戳滞后。
  3. 文档记录 damage 节流/合并策略及默认 24fps 间隔，计划文件补充执行状态。
- **影响**：在窗口/鼠标频繁变动场景下实际帧率受 `target_interval` 约束，可降低带宽和 CPU 占用；stop/wakeup pipe 流程保持不变。

## 2025-12-05：system handover 队列限流
- **目的**：及时清理长期无人领取的 handover 对象，并限制 pending 队列规模，降低 system 模式遭遇连接洪泛时的内存与 DBus 资源压力。
- **范围**：`src/system/drd_system_daemon.c`、`doc/architecture.md`、`.codex/plan/multi-module-optimization.md`。
- **主要改动**：
  1. `DrdRemoteClient` 的 `last_activity_us` 统一在 delegate 重连、RequestHandover、StartHandover、TakeClient、SessionReady 等事件中刷新，`drd_system_daemon_prune_stale_pending_clients()` 会清理静默超过 30 秒 (`DRD_SYSTEM_CLIENT_STALE_TIMEOUT_US`) 的 pending 对象并打印日志。
  2. `drd_system_daemon_queue_client()` 在入队前执行过期清理，并依据 `DRD_SYSTEM_MAX_PENDING_CLIENTS` 限制排队数量为 32，超过上限时拒绝手头的连接、释放 DBus 对象并提示管理员。
  3. `RequestHandover` 在派发对象前同样执行一次过期清理，文档同步记录“队列保护”策略，计划文件标记 system 阶段的执行进度与 PAM 优化延后。
- **影响**：system 守护在无人领取 handover 时不再无限积压对象，恶意连接无法占满 pending 队列，dispatcher/handover 可以稳定服务活跃用户。

## 2025-12-05：核心运行时与 system D-Bus 优化
- **目的**：降低传输模式切换与 SurfaceBits 回退的开销，并让 system 守护在失去 DBus 名称时自动降级退出，避免进程悬挂。
- **范围**：`src/core/drd_server_runtime.c`、`src/session/drd_rdp_session.c`、`src/system/drd_system_daemon.c`、`doc/architecture.md`、`doc/changelog.md`、`.codex/plan/project-module-optimization.md`。
- **主要改动**：
  1. `drd_server_runtime_set_transport()` 删除 `GMutex`，改用原子 CAS 判定实际发生的模式切换，仅在状态变化时触发关键帧，避免渲染/事件线程互斥。
  2. `DrdRdpSession` 缓存 `FreeRDP_MultifragMaxRequestSize`，渲染线程读取原子值即可决定 SurfaceBits 分片大小；退回日志降为 `DRD_LOG_DEBUG` 并附带 payload 限制。
  3. system 守护的 `g_bus_own_name_on_connection()` 现在附带 acquired/lost 回调并引用计数 `self`，丢失名称时立即请求主循环退出，同时移除多余的 `g_bus_own_name()` 调用。
  4. 架构文档新增“传输模式与 SurfaceBits 回退”小节及状态图，记录 CAS/缓存策略；计划文件同步最新步骤状态。
  5. `DrdX11Input` 内部增加 512 槽位的键码缓存与重置逻辑，同时将指针缩放比例改为在尺寸更新时预计算，减少高频键鼠事件路径上的重复查表与除法。
  6. `DrdEncodingManager` 在连续触发 SurfaceBits payload 超限后进入“raw grace” 周期，暂时跳过 RFX 压缩并直接输出 Raw，若客户端提高 `MultifragMaxRequestSize` 则自动清除并记录日志，同时累计回退次数以便定位带宽瓶颈。
  7. `DrdX11Input` 补齐 Unicode 注入能力，直接将 RDP Unicode 事件映射到 KeySym/KeyCode 并通过 XTest 注入，常见控制字符也能正确落地。
  8. `DrdX11Capture` 引入 wakeup pipe + `g_poll()`，stop 时写入管道即可唤醒捕获线程并快速退出，避免 `XNextEvent` 长时间阻塞；文档同步补充该机制。
  9. `DrdRdpSession` 渲染线程在 Rdpgfx 等待时加入 1 秒超时，超时即自动回退 SurfaceBits，并统计 `drd_server_runtime_pull_encoded_frame()` 的错误次数，在断开时输出累计值，便于定位 capture/encoding 瓶颈。
  10. `DrdFrameQueue` 从单帧缓存改为 3 帧环形缓冲，记录丢帧次数并提供查询函数，push 时超限会丢弃最旧帧，减少 encoder 赶不上 capture 时的刷新压力；文档同步说明该机制。
  11. `DrdSystemDaemon` 新增 pending/client 数量统计接口，并在注册 handover 客户端时输出当前总数，后续可用于限流和观测。
- **影响**：切换 Rdpgfx/SurfaceBits 时不再因为互斥锁阻塞编码，SurfaceBits 回退不再刷屏；system 守护失去 DBus 名称后会主动下线，由 systemd 自动重启，确保 Dispatcher/Handover 始终一致。

## 2025-12-04：RDP 监听器运行模式统一
- **目的**：消除 `DrdRdpListener` 内部的 `system_mode`/`handover_mode` 布尔散落点，统一由 `DrdRuntimeMode` 描述运行模式，降低调用方与监听器之间的状态组合复杂度。
- **范围**：`src/transport/drd_rdp_listener.[ch]`、`src/core/drd_application.c`、`src/system/drd_system_daemon.c`、`src/system/drd_handover_daemon.c`、`src/core/drd_config.{c,h}`、`doc/architecture.md`、`doc/changelog.md`、`.codex/plan/runtime-mode-listener.md`。
- **主要改动**：
  1. `DrdRdpListener` 结构体新增 `runtime_mode` 字段，`drd_rdp_listener_new()` 直接接收 `DrdRuntimeMode`，并据此控制被动会话、输入回调、delegate/cancellable 以及 handover 模式下的 RDSTLS 开关。
  2. `DrdApplication`、system/handover daemon 统一传入枚举值，避免自行维护布尔组合；旧的 `drd_config_get_system_mode()` API 被移除，保持配置层面对运行模式的单一来源。
  3. 架构/变更文档更新监听器职责描述，计划文件记录分析→重构→文档同步的执行情况。
- **影响**：监听器与外部控制器之间仅需记忆 `DrdRuntimeMode` 三态即可推导行为，后续扩展 handover 细分模式或新增测试模式时只需扩展枚举，不会再出现“布尔组合缺失/重复”的分支，system/handover 场景更易维护。

## 2025-12-03：握手触发 capture/编码链路
- **目的**：防止 handover/user 进程在没有客户端时就启动 `drd_x11_capture_thread`，并在断开后及时停止采集，保证重新连接能够重新拉起 capture/input/encoder。
- **范围**：`src/core/drd_server_runtime.*`、`src/core/drd_application.c`、`src/session/drd_rdp_session.c`、`src/transport/drd_rdp_listener.c`、`doc/architecture.md`、`doc/changelog.md`、`.codex/plan/runtime-stream-handshake.md`。
- **主要改动**：
  1. `DrdServerRuntime` 新增编码参数写入接口与 `stream_running` 标记，`drd_server_runtime_stop()` 仅在流处于活跃状态时停止 capture/input/encoder，避免重复启动。
  2. `drd_application_prepare_runtime()` 在 handover/user 模式下只写入编码参数，不再提前 `prepare_stream()`；`DrdRdpSession::Activate` 根据缓存的 `DrdEncodingOptions` 调用 `prepare_stream()`，失败会主动断开连接。
  3. `drd_rdp_listener_session_closed()` 在最后一个会话释放后触发 `drd_server_runtime_stop()`，断开立即停止 `drd_x11_capture_thread`，下次激活时重新启动；架构文档新增握手触发/停止序列图并更新数据流描述。
- **影响**：handover/user 进程在空闲时不再占用 XDamage/XShm 资源，断线后 capture 线程与编码状态都会释放，客户重新连接即可在下一次 Activate 时重新拉起流；日志也能清楚记录 “Runtime initialized without capture/encoding setup (awaiting session activation)”。

## 2025-12-03：Rdpgfx ACK suspend/resume 适配完善
- **目的**：补全提交 27f02bc8 中对 `SUSPEND_FRAME_ACKNOWLEDGEMENT` 的处理，防止客户端暂停 ACK 时 `outstanding_frames` 无限增长或在恢复后仍跳过背压。
- **范围**：`src/session/drd_rdp_graphics_pipeline.c`、`doc/architecture.md`、`doc/changelog.md`、`.codex/plan/optimize-27f02bc8.md`。
- **主要改动**：
  1. `DrdRdpGraphicsPipeline` 初始化与 reset 时显式重置 `frame_acks_suspended`，`can_submit`/`wait_for_capacity`/`submit_frame` 在同一把锁内判断该状态，暂停 ACK 时不再递增 `outstanding_frames`。
  2. `FrameAcknowledge` 回调根据 `queueDepth` 切换 suspend/resume，暂停时清空未确认帧并通过 `DRD_LOG_MESSAGE` 记录事件，恢复时重新启用背压并广播容量条件变量。
  3. `doc/architecture.md` 补充 `frame_acks_suspended` 状态机说明与 mermaid 状态图，明确 suspend/resume 对背压的影响；计划文件更新执行进度。
- **影响**：当 macOS 等客户端改以 `SUSPEND_FRAME_ACKNOWLEDGEMENT` 运行时，编码线程会即时解除阻塞但不会积累垃圾统计；客户端恢复 ACK 后重新从 0 计数，保持期望的三帧背压，日志也能清楚记录 suspend/resume 时刻，便于排障。

## 2025-12-01：概要设计 Typst 文档错别字与标点优化
- **目的**：清理 `doc/远程桌面概要设计.typ` 中的错别字、大小写与标点问题，使术语、流程描述和安全策略表述更精准。
- **范围**：`doc/远程桌面概要设计.typ`、`doc/changelog.md`、`.codex/plan/doc_typ_review.md`。
- **主要改动**：
  1. 统一 LightDM、Greeter、RDP Server Redirection 等术语写法，并在连接管理、服务重定向、远程会话章节补充更严谨的机制描述。
  2. 规范 CLI/配置参数、流程段落与列表的标点格式，修正 `UUID`、`DBus path`、`Redirect PDU` 等大小写及空格问题，消除多处英文/中文混用的半角符号。
  3. 调整人机交互、非功能性、部署章节的语句，使安全、性能、隐私条目以完整句式呈现，便于评审复用。
- **影响**：概要设计文档表达更清晰，关键流程对照实现细节更容易理解，后续在架构评审或需求讨论时可直接引用，减少语义歧义。

## 2025-12-01：architecture 文档同步概要设计
- **目的**：让 `doc/architecture.md` 与 `doc/概要设计.typ` 的结构、模块和流程保持一致，补充 LightDM/DBus 接口以及运行模式图，便于经验型开发者统一参考。
- **范围**：`doc/architecture.md`、`doc/changelog.md`、`.codex/plan/architecture-doc-refresh.md`。
- **主要改动**：
  1. 在架构文档开头新增设计原则与整体组件 mermaid 图，展示 drd-system/hand-over/user 与 LightDM、控制中心的交互关系。
  2. 扩展模块章节，补充服务重定向、远程会话/权限、配置与隐私、进程/虚拟屏幕/RDP seat/UI 管理等内容，并引入 CLI/INI/DBus 接口清单。
  3. 新增“关键流程”章节，使用 mermaid 序列图描述桌面共享、远程 Greeter 登录、远程 SSO 以及会话复用；同步记录计划文件进度。
- **影响**：架构文档可以直接映射到概要设计与现有实现，评审与需求讨论时无需在多个文件间跳转；新图表为后续 UML/Typst 导出奠定素材。

## 2025-12-01：概要设计关键结构与非功能章节优化
- **目的**：让概要设计对核心数据结构、性能与可靠性策略的描述与当前 C/GLib 实现保持一致，并为后续渲染 PlantUML 类图做好准备。
- **范围**：`doc/概要设计.typ`、`doc/uml/key-data-structures.puml`、`.codex/plan/文档优化.md`、`doc/changelog.md`。
- **主要改动**：
  1. 新增 `doc/uml/key-data-structures.puml`，用类图展示 `DrdServerRuntime`、`DrdRdpSession`、`DrdRemoteClient` 等之间的依赖关系，便于后续导出 PNG 并嵌入 Typst 文档。
  2. `doc/概要设计.typ` 在“关键数据结构设计”章节引用新图并扩写各结构字段/职责；“性能”与“可靠性”章节结合 runtime、Rdpgfx 背压、routing token 等实现细节重写描述，移除过时 TODO。
  3. `.codex/plan/文档优化.md` 记录任务背景与步骤，便于追踪执行过程；本文件同步登记改动。
  4. 补充 `DrdRdpListener` 与 DBus Handover 在类图中的关系，并在性能章节加入 Rdpgfx ACK 时序的 mermaid 片段与关键参数表，量化 16ms 拉帧、3 帧背压与 SurfaceBits 回退触发条件。
- **影响**：审阅概要设计即可了解 runtime、会话与 system 守护的职责切分与非功能策略，新建的 PlantUML 文件可复用到其他文档或 CI 产物中，减少口头同步成本。

## 2025-12-01：collect_dirty_rects 机制文档
- **目的**：沉淀 `collect_dirty_rects()` 的实现机制与性能优势，便于编码链路调优及 code review 时引用。
- **范围**：`doc/collect_dirty_rects.md`、`doc/architecture.md`、`.codex/plan/collect_dirty_rects分析.md`、`doc/changelog.md`。
- **主要改动**：
  1. 新增 `doc/collect_dirty_rects.md`，以哈希→校验→矩形输出为主线描述算法流程、性能收益与潜在优化方向，并附 mermaid 流程图。
  2. 架构文档在编码层章节补充 `collect_dirty_rects()` 摘要及交叉引用，方便读者找到详细设计说明。
  3. 计划文件记录调研-撰写-总结的执行状态，确保任务闭环；变更记录本身同步本次文档增量。
- **影响**：团队可直接引用文档理解 RFX 差分筛选逻辑，在分析编码瓶颈或复查脏矩形实现时有据可依，并为后续优化（如整块 `memcmp`）提供上下文。

## 2025-12-01：概要设计术语补充
- **目的**：让 `doc/概要设计.typ` 的术语说明章节覆盖当前实现中使用的核心缩写，避免评审时需要额外查阅资料。
- **范围**：`doc/概要设计.typ`、`.codex/plan/doc-terminology.md`、`.codex/plan/desktop-sharing-remote-login.md`。
- **主要改动**：
  1. 在术语列表中补充 SSO、PAM、PDU 的中文说明，并结合 drd-system、LightDM、Server Redirection 等上下文描述其作用。
  2. 补全“桌面共享”“远程登录”术语定义，区分 user 模式与 system+handover 模式的职责及安全策略。
  3. 建立并完成 `.codex/plan/doc-terminology.md` 与 `.codex/plan/desktop-sharing-remote-login.md` 任务记录，追踪调研、撰写与文档同步状态。
- **影响**：阅读概要设计即可理解远程单点登录、PAM 认证链路以及 RDP PDU 含义，后续讨论可直接引用该章节。

## 2025-11-25：system TLS 重连崩溃修复
- **目的**：system 监听器在一次 server redirection 后再次接受客户端时，FreeRDP 会访问上一段会话复用的 `rdpPrivateKey`，指针已经在前一次 `rdpSettings` 销毁时被释放，导致 `EVP_PKEY_up_ref()` 崩溃，需要确保每次握手都注入独立的证书/私钥对象。
- **范围**：`src/security/drd_tls_credentials.c`、`doc/architecture.md`、`doc/changelog.md`、`.codex/plan/system-reconnect-crash.md`。
- **主要改动**：
  1. `drd_tls_credentials_apply()` 不再直接重用缓存的 `rdpCertificate`/`rdpPrivateKey` 指针，而是根据 PEM 文本重新构造一份副本，并通过 `freerdp_settings_set_pointer_len()` 注入当次 `rdpSettings`，保证 FreeRDP 后续销毁的是本次握手拥有的对象。
  2. 失败路径补充错误信息，便于定位 TLS 物料缺失或 PEM 解析失败；system 端继续沿用同一份 PEM，但每次握手都会重新解析，避免跨会话共享 OpenSSL state。
  3. 架构文档“TLS 继承与缓存”章节新增 per-session TLS 副本说明，强调 system listener 复用 PEM 而非指针。
  4. 计划/变更记录同步当前修复进度，方便追踪 system 重连稳定性。
- **影响**：system 模式可以连续为多次 handover 周期接受初始连接，后续客户端再次连接 3389 端口不再在 `freerdp_tls_accept()` 内崩溃；handshake 过程中产生的证书/私钥副本由 FreeRDP 自动释放，不会污染缓存。

## 2025-11-25：certs 安装与构建精简
- **目的**：交付包需包含内置证书素材，同时减少中间静态库构建，缩短编译链路。
- **范围**：`data/meson.build`、`src/meson.build`、`README.md`、`doc/architecture.md`、`doc/changelog.md`、`.codex/plan/install-certs-no-static-libs.md`。
- **主要改动**：
  1. 在 Meson data 安装脚本中新增 `certs/` 的 `install_subdir`，安装路径为 `${datadir}/deepin-remote-desktop/certs/`，方便打包示例证书。
  2. 移除 `drd-media`/`drd-core` 静态库，`deepin-remote-desktop` 直接链接全部源文件，减少一次静态归档与链接步骤。
  3. README/架构文档同步模块说明与安装布局（提及 certs 安装位置与“单二进制”产物）。
- **影响**：安装产物包含所有默认证书和配置，构建阶段的目标数量下降，`meson compile` 直接产出主程序；打包脚本自动拾取新的数据目录，无需额外脚本。

## 2025-11-25：README 与 Debian 打包
- **目的**：向开发者说明最新的构建/安装路径，并提供官方 Debian 包装规则，方便交付或 CI 直接产出 `.deb`。
- **范围**：`README.md`、`debian/*`、`doc/architecture.md`、`doc/changelog.md`、`.codex/plan/debian-packaging.md`。
- **主要改动**：
  1. README 补充依赖列表、构建/测试/安装步骤，以及使用 `dpkg-buildpackage` 构建 `.deb` 的流程；安装部分说明 `meson install` 会落盘配置模板与 systemd unit。
  2. 新增 Debian 打包目录（`control/rules/changelog/install/docs/source/format`），并将 `debian/rules` 切换为 `DESTDIR=$(CURDIR)/debian/tmp) meson install -C obj-$(DEB_BUILD_GNU_TYPE)`，避免 Meson 默认把文件安装在 `obj-*/debian/tmp`。同时将可执行文件目标设为 `install: true`，不再手工复制。
  3. `.install`/`.docs` 继续归档配置模板、greeter drop-in、systemd unit 与文档，架构文档记录新的 Meson 安装策略。变更记录同步本次任务。
- **影响**：`dpkg-buildpackage` 可直接生成 `deepin-remote-desktop_0.1.0-1_*.deb`，内含服务单元/greeter drop-in/DBus policy 与 README/architecture/changelog 文档，部署无需额外脚本。

## 2025-11-25：Meson 安装路径调整
- **目的**：将 `data` 目录内的配置模板、greeter 脚本以及 systemd unit 在安装阶段投放到发行版要求的路径，避免包维护者手动复制。
- **范围**：`data/meson.build`、`doc/architecture.md`、`.codex/plan/meson-install-layout.md`、`doc/changelog.md`。
- **主要改动**：
  1. `data/meson.build` 新增 `install_subdir('config.d', ${datadir}/deepin-remote-desktop)`，并分别将 `11-deepin-remote-desktop-handover`、system/user unit 按照 `${sysconfdir}` 与 systemd unit 目录安装；systemd 路径优先读取 `pkg-config systemd` 变量，不存在时回退到 `prefix/lib/systemd/{system,user}`。
  2. 架构文档加入“部署与安装布局”小节，明确 config 模板、greeter drop-in 与 systemd unit 的落盘位置及 Meson 变量映射，同时补充数据流 mermaid 图。
  3. 计划文件更新执行进度，记录路径策略演进，变更记录本身补档。
- **影响**：发行版在运行 `meson install` 后即可拿到完整的配置模板与 service 单元，无需自定义脚本；路径与 `prefix/sysconfdir/datadir` 保持一致，方便 cross build 或根目录重定位。

## 2025-11-24：handover 模式动态凭据加载
- **目的**：解除 handover 运行对配置文件/CLI 中 TLS 与 NLA 账号的强制依赖，改为运行时获取一次性凭据。
- **范围**：`core/drd_config` 参数校验、`core/drd_application` 运行时准备、`system/drd_handover_daemon` 启动顺序、TLS 凭据管理。
- **主要改动**：
  - handover 模式下跳过 TLS 文件与静态 NLA 账号的校验，允许凭据在运行期注入。
  - 新增空的 `DrdTlsCredentials` 工厂，配合 dispatcher 返回的 PEM 数据即时加载证书。
  - handover 启动时先完成 DBus 绑定与 TLS 物料获取，再创建监听器，确保 `drd_rdp_listener` 使用最新凭据。


## 2025-11-23：handover TLS 继承与连接接管
- **目的**：handover 进程在 system 阶段已有客户端时重启，会直接复用本地证书与 `GSocketConnection` 自动指针，导致 TLS 身份与 system 下发的证书不一致、`TakeClient` 返回后发生二次 `g_object_unref()`。需要沿用 dispatcher 返回的证书/私钥，并修复连接所有权。
- **范围**：`src/security/drd_tls_credentials.*`、`src/system/drd_handover_daemon.c`、`doc/architecture.md`、`doc/changelog.md`、`.codex/plan/handover-nla-negotiation.md`。
- **主要改动**：
  1. `DrdTlsCredentials` 现在缓存最新的 PEM 证书/私钥，并新增 `drd_tls_credentials_reload_from_pem()`，允许 handover 直接在内存中替换 `rdpCertificate`/`rdpPrivateKey` 并向后续 `RedirectClient` 暴露一致的 TLS 材料。
  2. `drd_handover_daemon_start_session()` 对 dispatcher 返回的证书/私钥执行 reload，确保 handover 与 system 呈现同一 TLS 身份；`drd_handover_daemon_take_client()` 使用 `g_steal_pointer()` 把 `GSocketConnection` 的引用转移给 `DrdRdpListener`，消除 `g_object_unref: G_IS_OBJECT` 告警。
  3. 架构文档新增 TLS 继承与连接引用管理说明，并在变更记录与计划文件中标记本次修复。
- **影响**：handover 进程无须直接读取证书文件即可继承 system 的 TLS 身份，Server Redirection 后的客户端不会再因为证书漂移或连接被提前释放而在 CredSSP 阶段失败。

## 2025-11-22：system handover 日志写入崩溃修复
- **目的**：system 进程在 handover 重连/RedirectClient 高频日志期间会在 `drd_log_writer()` → `g_printerr()` 内触发 `g_convert()`，进一步进入 `iconv_open()` 和 `malloc/tcache`，出现重入崩溃。需要让日志写入路径脱离 `g_printerr`，保证在 GLib 日志锁中只执行可重入的最小逻辑。
- **范围**：`src/utils/drd_log.c`、`doc/architecture.md`、`.codex/plan/system-handover-log-writer.md`。
- **主要改动**：
  1. `drd_log_writer()` 改为使用 `GString` 拼接日志并直接 `write(STDERR_FILENO, …)` 输出，完全跳过 `g_printerr()`、locale 检测与 iconv 初始化；同时新增内部 `drd_log_write_stderr()`，以重试逻辑保证在 `EINTR` 下也不会截断输出。
  2. 日志章节补充“日志链路与观测”说明（含 mermaid 流程图），记录 `DRD_LOG_*` 宏→GLib structured log→writer→stderr 的完整链路，并明确本次修复目的。
  3. 更新 `.codex/plan/system-handover-log-writer.md`，标记堆栈分析、实现与文档同步节点，方便追踪后续反馈。
- **影响**：system/handover 在连接峰值或崩溃路径中不会再触发 `iconv_open()`，日志输出线程安全且无需额外堆分配，消除了重连两次 handover 后 `malloc/tcache` 触发的异常退出。

## 2025-11-21：remote id / routing token 互逆
- **目的**：system 模式仍将 handover 对象命名为 `session%u` 且依赖客户端携带的 cookie，初次连接无法生成 routing token，导致 StartHandover/RedirectClient 链路不稳定。需要对齐 upstream，通过 deterministic 映射让 remote_id 与 routing token 始终互逆。
- **范围**：`src/system/drd_system_daemon.c`、`doc/architecture.md`、`.codex/plan/remote-id-routing-token.md`。
- **主要改动**：
  1. 新增 `get_id_from_routing_token()` / `get_routing_token_from_id()` 与 `drd_system_daemon_generate_remote_identity()`，通过 `/org/deepin/RemoteDesktop/Rdp/Handovers/<token>` 模式一次生成 remote_id 与十进制 token，并拒绝 0 或重复 token。
  2. `drd_system_daemon_register_client()` 不再引用 peek 结构体，改为复制 `DrdRoutingTokenInfo`，若客户端未携带合法 token 即立即分配新 token；注册时始终把 token 写入 `DrdRemoteClient::routing->routing_token`，确保 StartHandover 可以发送 Server Redirection。
  3. `drd_system_daemon_find_client_by_token()` 直接将 cookie 解析成整数后调用互逆函数做 O(1) 哈希查找，拒绝格式错误 token 并重新生成，避免遍历 `remote_clients`。
  4. 文档新增 “Remote ID ↔ Routing Token 互逆” 小节，记录映射函数与新查找路径；计划文档补充任务背景与执行步骤。
- **影响**：system 守护在初次连接时即可拿到合法 routing token，StartHandover/RedirectClient 必然携带 cookie；二次连接只需解析十进制 token 即可命中特定 handover，减少 O(n) 查找。非法/重复 cookie 会即时重置并写日志，避免 handover 状态错乱。

## 2025-11-21：handover redirect 连接释放
- **目的**：第一个 handover 在发送 Server Redirection 后手动调用 `g_io_stream_close()` 会偶发崩溃，因为 FreeRDP 会话已经在内部销毁 socket。需要对齐 upstream，彻底移除 handover 对 socket 的直接管理。
- **范围**：`src/system/drd_handover_daemon.c`、`doc/architecture.md`、`.codex/plan/handover-redirect-close.md`。
- **主要改动**：
  1. handover 结构体移除 `active_connection` 字段，并新增主循环引用，RedirectClient 成功后会调用 `drd_handover_daemon_stop()` 与 `g_main_loop_quit()` 直接退出进程，全程依赖 `DrdRdpSession` 完成 socket 关闭。
  2. system 守护同样增加 `GMainLoop` 引用与 `drd_system_daemon_set_main_loop()`，`drd_system_daemon_stop()` 在释放资源后会请求退出主循环，方便 system 模式在致命错误或人工触发时优雅停机。
  3. 架构文档更新“连接关闭职责”说明，强调 handover 只触发 server redirection，socket 生命周期完全由会话控制且完成后立即交棒下一阶段；system 守护也具备同等的主循环退出能力。
  3. 记录计划文件，跟踪此次修复。
- **影响**：RedirectClient 流程不会再访问失效的 `GSocketConnection`，第二个 handover 启动时也不会触发崩溃，行为与 gnome-remote-desktop 一致。

## 2025-11-20：RedirectClient 与多阶段 handover
- **目的**：第二个 handover 进程在 `RequestHandover` 阶段收到 “No pending RDP handover requests”，且 `RedirectClient` 信号仅打印日志，导致 system 未能驱动 greeter→用户会话的二次重定向。
- **范围**：`src/system/drd_system_daemon.c`、`src/system/drd_handover_daemon.c`、`src/security/drd_tls_credentials.*`、`doc/architecture.md`、`.codex/plan/system-redirectclient.md`。
- **主要改动**：
  1. `DrdRemoteClient` 引入 `handover_count`，`drd_system_daemon_on_take_client()` 第一次 `TakeClient()` 后不再移除对象，而是重新排队等待下一段 handover；第二次领取后才真正 `remove_client()`，确保用户会话能复用同一 object path。
  2. 新增 `drd_tls_credentials_read_material()`，system/handover 均复用该助手载入 PEM 证书/私钥，避免重复 I/O 逻辑。
  3. handover 守护挂接 listener session 回调并缓存当前 `DrdRdpSession`，收到 `RedirectClient` 信号时调用 `drd_rdp_session_send_server_redirection()` + `drd_rdp_session_notify_error()`，同步关闭本地 socket，真正驱动客户端重连。
  4. 文档新增多阶段 handover 队列与 RedirectClient 执行链说明，记录 system delegate → handover → client 的完整重定向路径。
- **影响**：第二个 handover 进程能够顺利获取待接管对象，system 在 `StartHandover` 时会唤起现有 handover 发送 Server Redirection，客户端重连后由新 handover 领取 FD，远程登录流程符合 “System → Greeter → 用户” 的双阶段时序。

## 2025-11-20：handover 重连短路监听器
- **目的**：修复 StartHandover 后客户端重连仍再次落到 system 进程、触发 `peer->CheckFileDescriptor()` 崩溃的问题，确保 handover delegate 抢占的连接不会被默认监听逻辑重复处理。
- **范围**：`src/transport/drd_rdp_listener.c`、`doc/architecture.md`、`.codex/plan/handover-reconnect-crash.md`。
- **主要改动**：
  1. `drd_rdp_listener_incoming()` 当 delegate 返回 handled 或产生错误时立即短路，不再继续走默认 `drd_rdp_listener_handle_connection()`，避免对 handover 重连 socket 二次创建 FreeRDP 会话。
  2. 当 delegate 自身返回错误时保持原有语义——记录日志并停止后续处理，确保 system 监听器不会在半初始化的 socket 上继续初始化。
  3. 架构文档补充监听短路说明，强调 system delegate 与默认监听链路之间的职责边界。
- **影响**：handover 守护在 StartHandover → Redirect → 客户端重连后不再遇到重复进入 `drd_rdp_listener_incoming()` 的 FreeRDP 初始化流程，system 进程不会二次接管连接，从而消除 `peer->CheckFileDescriptor()` 崩溃。

## 2025-11-20：routing token peek 对齐 upstream
- **目的**：system 模式在 StartHandover 之后无法触发重定向，原因是 `drd_routing_token_peek()` 只读取 11 字节导致 `Cookie: msts=` 永远解析失败，routing token 始终为空，系统端跳过 `RedirectClient`。需要将解析流程与 upstream `peek_routing_token()` 保持一致，确保二次连接能拿到 token 并触发 `TakeClientReady`。
- **范围**：`src/transport/drd_rdp_routing_token.c`、`src/system/drd_system_daemon.c`、`doc/architecture.md`、`.codex/plan/takeclientready.md`。
- **主要改动**：
  1. `drd_routing_token_peek()` 读取 TPKT header 后再一次性 peek 完整 payload，校验 x224 字段并用 `\r\n` 边界提取 `Cookie: msts=`，随后解析 `rdpNegReq` 中的 `requested_protocols`，完整复制 upstream 逻辑；若解析失败会返回 GIO 错误并保留 socket，避免 silent fallback。
  2. 首次连接缺失 routing token 时，system daemon 会随机生成十进制 token 并写入 `DrdRemoteClient`，以便在 `StartHandover` 阶段通过 Server Redirection PDU 主动推送给客户端；重连后即可携带 `Cookie: msts=<token>` 触发 `TakeClientReady`。
  3. routing token 解析失败时会清空之前缓存的 token，防止错误值污染后续 handover；同时在无 cookie 的初次连接上保持兼容，让 StartHandover 仍可下发 TLS 凭据。
  4. 架构文档更新 Routing Token 章节，说明新的解析顺序、`RDSTLS` 捕获方式以及本地 token 生成策略；计划文档记录本次修复进度。
- **影响**：Server Redirection PDU 再次可用——首次连接即便没有 `Cookie: msts=`，system daemon 也会生成 token 并在 StartHandover 下发；客户端依据该 token 重连后，system delegate 能匹配已注册的 handover 并发出 `TakeClientReady`，handover daemon 随即调用 `TakeClient()` 领取 socket，系统端和 handover 端连接闭环可恢复。

## 2025-11-21：system handover 路由 token socket 修复
- **目的**：system 模式监听端口在 peek routing token 后立即出现 `g_socket_is_connected` 断言并拒绝连接，需要修复 socket 生命周期，确保 handover 注册阶段不会破坏 TCP 会话；同时修正 handover 队列在 delegate 返回 FALSE 时被提前清空的问题。
- **范围**：`src/transport/drd_rdp_routing_token.c`、`src/system/drd_system_daemon.c`、`doc/architecture.md`、`.codex/plan/system-socket-handshake.md`、`.codex/plan/system-handover-queue.md`。
- **主要改动**：
  1. `drd_routing_token_peek()` 不再用 `g_autoptr(GSocket)` 自动 `unref`，改为借用 `GSocketConnection` 持有的 socket 指针并加注释说明原因，防止 system 守护 peek 完毕后销毁底层 `GSocket`。
  2. `drd_system_daemon_delegate()` 在成功注册 handover client 后直接返回 TRUE，阻止默认监听器继续初始化 `freerdp_peer`，保护 pending 队列中的连接不被提前关闭；新增计划文档跟踪该问题。
  3. `drd_system_daemon_on_start_handover()` 允许缺失 routing token 的客户端继续完成调用，只在存在既有 session 时才强制要求 token；缺口情况下跳过 `RedirectClient` 信号，提示 handover 直接 `TakeClient`。
  4. `drd_system_daemon_delegate()` 在首次注册 handover 客户端后不再吞掉连接，而是让 `DrdRdpListener` 继续创建 `DrdRdpSession`，确保 system 端能够发送 Server Redirection PDU；仅在匹配已有 routing token 的二次连接时才拦截并立即触发 `TakeClientReady`。
  5. 架构文档在 Routing Token 与运行模式章节记录 socket 生命周期、delegate 行为以及无 token 时的兼容策略，提醒开发者在 peek 与 handover 阶段避免释放底层 socket。
- **影响**：system 模式在注册 handover 客户端时不再触发 GLib/GIO 断言，`drd_rdp_listener_incoming` 能继续交由 handover 流程处理连接，后续 `freerdp_peer_new()` 可以成功复制 fd；没有携带 routing token 的客户端仍能领取 TLS 证书并通过 `TakeClient` 抢占现有连接，只是无法收到服务器重定向信号。启用 Server Redirection 后，支持该功能的客户端会在收到 PDU 后主动重连，system 守护在二次连接上匹配 routing token 并发出 `TakeClientReady`。

## 2025-11-20：运行模式与 handover 框架对齐
- **目的**：对齐 gnome-remote-desktop 的 system/handover 设计，为 system bus handover、routing token 重定向以及后续 LightDM 单点登录打好地基。
- **范围**：`core/drd_config.*`、`core/drd_application.c`、`transport/drd_rdp_listener.*`、`transport/drd_rdp_routing_token.*`、`system/drd_system_daemon.*`、`system/drd_handover_daemon.*`、`doc/architecture.md`、`.codex/plan/system-handover.md`。
- **主要改动**：
  1. `DrdRuntimeMode` 将 CLI/配置统一成 user/system/handover 三态；`--mode=`/`[service] runtime_mode` 线上实时切换，system/handover 模式下跳过原有采集/编码路径并托管给新的守护类。
  2. `DrdRdpListener` 支持 delegate + adopt API，system 守护可在 `incoming` 前窥探连接，自行注册 handover 对象；handover 守护可在接收 Unix FD 后复用原有监听器流程。
  3. 新增 `DrdRoutingTokenInfo`，在 TLS/TPKT 握手阶段读取 `Cookie: msts=` 与 `RDSTLS` 标志，为 DBus handover 对象提供 routing token。
  4. system 守护导出 `org.deepin.RemoteDesktop.Rdp.Dispatcher/Handover` skeleton，handover 守护通过 Request/Start/TakeClient 请求 socket fd；StartHandover 会根据是否存在活跃 session 决定发送 Server Redirection PDU 还是向已存在的 handover 转发 `RedirectClient` 信号，重连成功后通过 `TakeClientReady/TakeClient` 将新的 socket FD 交给目标 handover。
  5. 架构文档增加运行模式、system/handover 数据流与 mermaid 序列图，强调 LightDM 尚未提供 SSO API，本轮仅实现 socket 调度与 DBus 框架；`data/org.deepin.RemoteDesktop.conf` 引入 system bus policy，安装到 `/etc/dbus-1/system.d/`，只允许 `root` 用户占有 `org.deepin.RemoteDesktop*`，并开放 Dispatcher/Handover 接口给默认 context。
- **影响**：system 模式可以注册/排队多个待 handover 的客户端，并通过 routing token 触发二次重定向；handover 模式以普通用户身份运行，拿到 fd 后立刻复用现有 RDP 会话初始化逻辑。PAM 单点登录暂未介入——需要等待 deepin 桌面（lightdm）暴露统一认证接口。

## 2025-11-19：监听层切换至 GSocketService
- **目的**：复用 gnome-remote-desktop 的 system/handover 思路，摆脱 `freerdp_listener` 轮询模型，为 system 模式后续扩展 routing token 做准备。
- **范围**：`src/transport/drd_rdp_listener.*`、`doc/architecture.md`、`.codex/plan/gsocket-listener.md`。
- **主要改动**：
  1. `DrdRdpListener` 继承 `GSocketService`，通过 `g_socket_listener_add_inet_port()`/`add_address()` 绑定端口，`incoming` 回调里将 `GSocketConnection` 的 fd 复制给 `freerdp_peer`，并保留原有 TLS/NLA/输入初始化与 `DrdRdpSession` 生命周期钩子。
  2. 移除 `freerdp_listener`/tick loop 相关字段与定时器，停止流程改为 `g_socket_service_stop()+g_socket_listener_close()`，并在 system 模式下预创建 `GCancellable`，为 handover 路径保留取消入口。
  3. 文档补充新的监听架构序列图和 `incoming` 状态转换，计划文件同步记录任务背景与五步执行路径。
- **影响**：监听端口完全托管给 GLib 主循环，system 模式不再依赖 FreeRDP 轮询线程，后续可直接套接 routing token/DBus handover；同时端口绑定失败时能即时获得 GIO 错误，而非沉默失败。

## 2025-11-18：认证流程收敛
- **目的**：仅保留“NLA 开启 / NLA 关闭 + PAM 单点登录”两条路径，移除 delegate 相关代码和配置。
- **范围**：`core/drd_config.*`、`core/drd_application.c`、`transport/drd_rdp_listener.*`、`session/drd_rdp_session.*`、`config/*.ini`、`config/deepin-remote-desktop.service`、`README.md`、`doc/architecture.md`、`.codex/plan/rdp-security-overview.md`。
- **主要改动**：
  1. 新增 `[auth] enable_nla` / `--enable-nla` / `--disable-nla`，默认开启 CredSSP，关闭时自动切换至 TLS+PAM；`[service] rdp_sso` 仅作为兼容别名。
  2. 删除 delegate 模式、FreeRDP `Logon` 回调及 `drd_rdp_session_handle_logon()`，监听器仅在 `enable_nla=false` 时读取 Client Info 凭据并调用 PAM。
  3. 简化 RDP 监听/会话结构，去除委派状态与凭据擦除辅助函数，TLS 路径改为日志脱敏并直接挂接到 `drd_local_session`。
  4. 示例配置与 service unit 统一描述 NLA on/off 两种模式，文档/README/计划文件同步更新。
- **影响**：NLA 关闭时不再需要在配置里重复写用户名/密码，客户端凭据直接进入 PAM 并开启对应用户会话；delegate 场景彻底下线，部署和运维只需关注是否启用 NLA。

## 2025-11-16
- **目的**：将项目目标更新为“Linux 上的现代 RDP 服务端”，补充功能蓝图与缺口，确保文档与现状对齐。
- **范围**：
  - `doc/architecture.md`：重写愿景与当前能力，新增现代 RDP 功能蓝图（含 mermaid 图）、虚拟通道/服务化规划，以及短期优化与能力缺口清单。
  - `.codex/plan/architecture-modern-rdp.md`：记录本次文档任务的上下文与进度。
- **影响**：团队明确当前完成度与后续方向，评审与规划可直接引用文档蓝图；后续特性（音频、剪贴板、H.264、服务化等）有清晰落差列表。

## 2025-11-13
- **目的**：对齐 `gnome-remote-desktop` 的 Progressive 帧发送约束，避免 Rdpgfx 在首帧未携带 Header 时输出方块/灰屏。
- **范围**：
  - `glib-rewrite/src/encoding/drd_rfx_encoder.c`：关键帧编码路径现在准确标记 `DrdEncodedFrame` 的 `is_keyframe` 状态，为下游判定提供依据。
  - `glib-rewrite/src/session/drd_rdp_graphics_pipeline.{c,h}`：新增 `DRD_RDP_GRAPHICS_PIPELINE_ERROR_NEEDS_KEYFRAME`，在 `needs_keyframe` 置位且收到非关键帧时拒绝提交，并在提交失败后重置该标志，确保下一帧重新携带 Progressive Header；提交接口同步接收调用方提前读取的 `frame_is_keyframe`，防止编码线程重用对象造成判定竞态。
  - `glib-rewrite/src/session/drd_rdp_session.c`：新增 renderer 线程，在会话激活后用专用线程串行执行 “拉帧→编码→发送”，接收到关键帧缺失错误时调用 `drd_server_runtime_request_keyframe()`，无需禁用 Rdpgfx 即可重新对齐。
  - `glib-rewrite/src/core/drd_server_runtime.c`：移除异步编码线程与 `encoded_queue`，`drd_server_runtime_pull_encoded_frame()` 改为同步等待捕获帧并立即编码，供 renderer 线程消费。
  - `doc/architecture.md`：记录关键帧守卫、RLGR1 Progressive、FrameAcknowledge 背压以及 renderer/捕获线程协作流程，便于团队理解与排查。
- **影响**：首次或重置后的 Progressive 帧一定是带 Header 的关键帧，即使编码线程存在竞态也会被拦截，客户端不再出现灰屏；遇到竞态时仅触发一次重新编码，不会强制回退 SurfaceBits。

### Contributor Guide（AGENTS.md）
- **目的**：为贡献者提供集中化的开发手册，涵盖目录结构、构建/测试命令、编码规范及提交流程，缩短上手时间。
- **范围**：
  - 新增 `AGENTS.md`，以 “Repository Guidelines” 形式记录模块划分、Meson 命令、命名约定、测试策略以及安全注意事项。
  - 在 `.codex/plan/contributor-guide.md` 建立并更新任务计划，跟踪调研、撰写与文档同步节点。
- **影响**：仓库现在具备 200-400 词的英文 Contributor Guide，可直接引用于 code review/PR 模板，并确保后续任务能够通过 `.codex/plan` 跟踪状态。

### 架构文档校验
- **目的**：将 `doc/architecture.md` 与当前 C/GLib 实现对齐，移除已废弃的 “编码线程/GAsyncQueue/DrdRdpRenderer” 描述，并突出同步编码与 renderer 线程的真实职责。
- **范围**：
  - 更新模块分层、数据流、FrameAcknowledge 章节，描述 `drd_server_runtime_pull_encoded_frame()` 的同步编码流程、`drd_rdp_session_render_thread()` 的发送策略及 `max_outstanding_frames` 背压机制。
  - 修正 Rdpgfx 背压图示与文字，删除对 `DrdRdpRenderer` 文件、`doc/rdpgfx-pipeline.md` 的引用，并说明当前 ACK 仅递减 `outstanding_frames`。
  - 新增 “待优化方向” 小节，列出 ACK 无限等待、Unicode 注入缺失以及多显示器拓扑空白等待办事项。
- **影响**：架构文档与源码状态一致，开发者可依文档理解渲染线程/背压行为，并明确下一步应改进的薄弱点。

## 2025-11-14
- **目的**：补充最新的 RDPGFX 流水线文档，确保 `doc/architecture.md`/`doc/changelog.md` 能准确描述 RLGR1 Progressive、renderer 单线程、FrameAcknowledge 背压以及 capture → encode → send 的协作关系。
- **范围**：
  - `doc/architecture.md`：新增 “单线程编码与发送”“FrameAcknowledge 与 Rdpgfx 背压”“Renderer & Capture 线程协作” 等小节，引用 `doc/rdpgfx-pipeline.md` 的数据流，说明 `needs_keyframe`、`capacity_cond`、RLGR1 配置与 renderer 线程行为。
  - `doc/changelog.md`：记录上述文档更新，明确 Progressive RFX 的默认 RLGR1、关键帧守卫及调试方法。
- **影响**：开发者查阅文档即可了解当前实现的关键约束（ACK 背压、关键帧要求、renderer 生命周期），无需再回溯旧的 GNOME upstream 描述即可调试 `glib-rewrite`。

### 会话重连修复
- **目的**：解决客户端首次断开后监听器仍认为存在活动会话、拒绝后续连接的问题，避免日志报错 `session already active`。
- **范围**：
  - `glib-rewrite/src/transport/drd_rdp_listener.c`：`DrdRdpPeerContext` 持有监听器引用，`drd_peer_disconnected()` 与 `drd_peer_context_free()` 都会调用新的 `drd_rdp_listener_session_closed()`，在断线及异常析构时清理 `sessions` 数组并输出剩余会话数。
  - `glib-rewrite/src/session/drd_rdp_session.{c,h}`：新增 `drd_rdp_session_set_closed_callback()`，在 VCM 线程退出或停止事件线程后触发一次性回调，让监听器即时移除僵尸会话，即便 FreeRDP 没有触发 `Disconnect` 也能恢复可连接状态。
  - `.codex/plan/rdp-reconnect.md`：建立并更新排查计划，记录根因、修复与验证过程，便于后续追溯。
  - `doc/architecture.md`：新增 “会话生命周期与重连” 小节及状态图，描述监听器如何串联 FreeRDP 回调来维护单会话限制。
- **影响**：FreeRDP 回调会在任何断线路径上释放 `g_ptr_array` 中的会话引用，监听器能立即接受新客户端，避免 BIO 重试耗尽及 `PeerAccepted` 失败；同时文档对调度逻辑有清晰记录，方便后续维护。

### 扩展扫描码输入修复
- **目的**：修复客户端方向键等扩展扫描码在服务端被视为 >256 而报错 `freerdp_keyboard_get_x11_keycode_from_rdp_scancode` 的问题，并补上 Alt/AltGr 等修饰键的映射兜底。
- **范围**：
  - `glib-rewrite/src/input/drd_x11_input.c`：为键盘注入路径新增 `<freerdp/scancode.h>` 依赖，并在调用 `freerdp_keyboard_get_x11_keycode_from_rdp_scancode()` 时仅传递 8-bit scan code，独立携带 `extended` 标记，避免 0xE0 前缀直接累加后超界；当 FreeRDP 旧映射返回 0（如 Alt/AltGr），自动回退到基于 `XKeysymToKeycode()` 的键值查找，确保修饰键也能注入。
  - `doc/architecture.md`：输入层章节记录扩展扫描码处理方式及 Alt 兜底逻辑。
  - `.codex/plan/keyboard-scancode.md`：创建并完成对应计划，便于后续追溯。
- **影响**：方向键、Ins/Del 等扩展键可正常注入 X11，FreeRDP 日志不再出现 “ScanCode XXX exceeds allowed value range [0,256]”。

## 2025-11-12
- **目的**：整理 Rdpgfx Progressive 拥塞/花屏调查计划，明确分析上下文。
- **范围**：新增 `.codex/plan/rdpgfx-progressive-congestion-analysis.md`，记录任务背景与分步计划，后续用以比对 `gnome-remote-desktop` 与 `glib-rewrite` 的图形管线差异。
- **影响**：便于跟踪分析进度并向团队同步关键节点，减少重复摸索。

### Rdpgfx 背压修复
- **目的**：彻底解决 “dropping progressive frame due to Rdpgfx congestion” 导致的花屏问题，确保 Progressive 帧只有在客户端确认后才继续编码。
- **范围**：
  - `glib-rewrite/src/session/drd_rdp_graphics_pipeline.c`：新增 `capacity_cond` 条件变量与 `drd_rdp_graphics_pipeline_wait_for_capacity()`，在 FrameAcknowledge 触发后唤醒等待线程。
  - `glib-rewrite/src/session/drd_rdp_renderer.{c,h}` + `drd_rdp_session.c`：引入专用 renderer 线程与帧队列，所有 Progressive 帧均在该线程串行提交；如 200 ms 内未获 ACK，会通过回调禁用管线并强制关键帧后回退 SurfaceBits。
  - `glib-rewrite/src/session/drd_rdp_graphics_pipeline.c`：`ResetGraphics` 现在携带完整的 `MONITOR_DEF` 列表，确保客户端正确建立显示布局，避免因 monitorCount=0 导致的灰屏。
  - `doc/architecture.md`：记录新的背压机制与交互时序。
- **影响**：Rdpgfx 拥塞时不再直接丢帧，客户端与服务器保持同步视图，花屏现象消失，同时通过关键帧重置避免脏块继续引用失效帧。

## 2025-11-11
- **目的**：修复 `rdpgfx_context->Open()` 在 `glib-rewrite` 中阻塞的问题，并记录新的 RDPGFX 初始化约束。
- **范围**：调整 `glib-rewrite/src/session/drd_rdp_graphics_pipeline.c` 中的锁策略，补充 `doc/architecture.md` 对 VCM 线程与 Rdpgfx 管线关系的说明。
- **影响**：Rdpgfx 管道在握手阶段不再自锁，`ChannelIdAssigned`/`CapsAdvertise` 能顺利回调并建立 surface，提升 RFX Progressive 路径的可用性。

### Progressive 花屏修复
- **目的**：解决 Progressive RFX 画面花屏问题，确保编码输出符合 RDPEGFX 规范。
- **范围**：在 `glib-rewrite/src/encoding/drd_rfx_encoder.c` 中新增手写 Progressive 帧封装函数，移除 `progressive_rfx_write_message_progressive_simple()` 依赖，并更新 `doc/architecture.md` 记录差异。
- **影响**：客户端可以解析完整的 `SYNC/CONTEXT/REGION/TILE` 元数据，消除颜色错位，避免与 Windows RDP 客户端产生兼容性问题。
- **进一步修复**：当 Rdpgfx 管线拥塞或提交失败时，不再回退到 SurfaceBits 复用同一 RFX Progressive payload；改为丢弃当前帧并在必要时禁用图形管线（`glib-rewrite/src/session/drd_rdp_session.c`）。这样可避免 Progressive 数据被 SurfaceBits 路径误解导致的 TLS 错误与客户端花屏。
- **SurfaceFrameCommand 对齐**：`drd_rdp_graphics_pipeline_submit_frame()` 现在优先调用 FreeRDP 的 `SurfaceFrameCommand`，自动封装 `StartFrame/Surface/End` 并确保 FrameAcknowledge 正常触发；仅在旧库缺少该入口时回退到手动序列（`glib-rewrite/src/session/drd_rdp_graphics_pipeline.c`）。
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

# 2025-11-16：CredSSP 凭据委派与 system 模式
- **范围**：`core/drd_config.*`、`core/drd_application.c`、`transport/drd_rdp_listener.*`、`session/drd_rdp_session.*`、`security/drd_local_session.*`（新增）、`config/default.ini`、`config/deepin-remote-desktop.service`、`doc/architecture.md`、`.codex/plan/rdp-security-overview.md`。
- **主要改动**：
  1. 配置层新增 `nla-mode`（static/delegate）、`--system` 开关与 PAM service 管理，delegate 模式自动启用 CredSSP 凭据委派且要求 root/systemd 托管。
  2. `DrdRdpListener`/`DrdRdpSession` 挂载 FreeRDP `Logon` 回调，使用新模块 `drd_local_session` 与 PAM 建立 per-user 会话，失败时直接拒绝连接。
  3. static 模式继续使用一次性 SAM 文件；delegate 模式关闭 `FreeRDP_NtlmSamFile`，在 `PostConnect` 后擦除凭据并在断开时调用 `pam_close_session`。
  4. CLI/INI 提供 `--nla-mode`、`--system` 以及 `--enable-rdp-sso`/`[service] rdp_sso`，配置日志输出 NLA 模式信息；`config/deepin-remote-desktop.service` 演示 systemd unit，在 system 模式下可选择保留 CredSSP（默认）或退回 TLS-only RDP 单点登录（直接读取 Client Info + PAM），且都跳过 X11 捕获/编码。
  5. 文档更新安全链路、模块分层与 system 模式说明，新增 CredSSP → PAM 序列图并记录计划完成情况。
- **影响**：RDP 客户端在 `delegate` 模式下可直接使用输入的用户名/密码完成网络鉴权 + PAM 登录，实现“一次输入”体验。`--system` 由 systemd/root 托管，普通非 root 无法误启特权进程。

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
