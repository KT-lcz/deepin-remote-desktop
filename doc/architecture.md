# GLib Codex 重构架构概览

## 总体目标
- 以 C(GLib/GObject) 为核心，重构原 C++ RDP 服务端，保证单一职责、易扩展。
- 构建模块化层次：应用入口 → 运行时 → 采集/编码/输入 → 传输(RDP) → 对外接口。
- 在重构过程中逐步替换旧逻辑，避免新旧代码交叉依赖。

## 模块分层

### 1. 核心层
- `core/grdc_application`：负责命令行解析、GLib 主循环、信号处理与监听器启动（打包进 `libgrdc-core.a`）。
- `core/grdc_server_runtime`：聚合 Capture/Encoding/Input 子系统，提供 `prepare_stream()` / `stop()` 接口，并在内部启动编码线程，从抓屏队列取帧并通过 `GAsyncQueue` 将编码帧传递给会话层。
- `core/grdc_config`：解析 INI/CLI 配置，集中管理绑定地址、TLS 证书、捕获尺寸等运行参数。
- `security/grdc_tls_credentials`：加载并缓存 TLS 证书/私钥，供运行时向 FreeRDP Settings 注入。

### 2. 采集层
- `capture/grdc_capture_manager`：启动/停止屏幕捕获，维护帧队列。
- `capture/grdc_x11_capture`：X11/XShm 抓屏线程，侦听 XDamage 并推送帧。
（以上与编码/输入/工具组成 `libgrdc-media.a`，供核心库复用）

### 3. 编码层
- `encoding/grdc_encoding_manager`：统一编码配置、调度；对外暴露帧编码接口。
- `encoding/grdc_raw_encoder`：原始帧编码器（BGRX → bottom-up），兼容旧客户端。
- `encoding/grdc_rfx_encoder`：基于 RemoteFX 的压缩实现，支持帧差分、质量档位与底图缓存，为后续的多片段/差分优化提供基础。

### 4. 输入层
- `input/grdc_input_dispatcher`：键鼠事件注入入口，管理 X11 注入后端与 FreeRDP 回调。
- `input/grdc_x11_input`：基于 XTest 的实际注入实现，负责键盘、鼠标、滚轮事件。

### 5. 传输层
- `transport/grdc_rdp_listener`：FreeRDP 监听生命周期、Peer 接入、会话轮询。
- `session/grdc_rdp_session`：会话状态机，维护 peer + runtime 引用，通过 SurfaceBits 推送编码帧，封装键鼠事件注入入口。

### 6. 通用工具
- `utils/grdc_frame`：帧描述对象，封装像素数据/元信息。
- `utils/grdc_frame_queue`：线程安全的单帧阻塞队列。
- `utils/grdc_encoded_frame`：编码后帧的统一表示，携带 payload 与元数据。

## 数据流简述
1. 应用启动后创建 `GrdcServerRuntime`，准备编码器与采集模块。
2. `GrdcCaptureManager` 启动 `GrdcX11Capture`，持续推送 `GrdcFrame`；运行时编码线程消费该队列并将 `GrdcEncodedFrame` 写入异步队列。
3. 会话层从编码队列拉取帧，经 `SurfaceFrameMarker`/`SurfaceBits` 推送给客户端，并在无帧时保持事件响应。

## 设计原则落实
- **SOLID**：各模块限定单一职责；监听器依赖抽象的 runtime；待迁移的编码/输入将通过接口剥离具体实现。
- **KISS/YAGNI**：阶段性仅实现最小可运行路径（监听 + 采集），编码/输入按需延伸。
- **DRY**：帧结构、队列作为共享组件供捕获/编码/传输复用。
