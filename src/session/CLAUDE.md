# session 模块 - 会话管理

[根目录](../../CLAUDE.md) > [src](../) > **session**

## 模块职责

session 模块负责：
- RDP 会话状态机管理
- FreeRDP peer 生命周期
- 图形管线初始化
- 会话激活/断开流程

## 入口与启动

### 会话创建

- **头文件**: `drd_rdp_session.h`
- **创建**: `drd_rdp_session_new(freerdp_peer *peer)`
- **设置**:
  - `drd_rdp_session_set_runtime()` - 关联运行时
  - `drd_rdp_session_set_virtual_channel_manager()` - 设置 VCM
  - `drd_rdp_session_set_closed_callback()` - 注册关闭回调
  - `drd_rdp_session_set_passive_mode()` - 被动会话标志（system 模式）

### 会话生命周期

```
new → post_connect → activate → pump/render → disconnect
```

- `post_connect()`: TLS/NLA 握手成功后调用
- `activate()`: 客户端激活会话
- `pump()`: 渲染线程运行前的基础循环
- `disconnect()`: 主动断开连接

## 对外接口

### DrdRdpSession 错误码

```c
typedef enum {
    DRD_RDP_SESSION_ERROR_NONE = 0,
    DRD_RDP_SESSION_ERROR_BAD_CAPS,
    DRD_RDP_SESSION_ERROR_BAD_MONITOR_DATA,
    DRD_RDP_SESSION_ERROR_CLOSE_STACK_ON_DRIVER_FAILURE,
    DRD_RDP_SESSION_ERROR_GRAPHICS_SUBSYSTEM_FAILED,
    DRD_RDP_SESSION_ERROR_SERVER_REDIRECTION,
} DrdRdpSessionError;
```

### 关键方法

- `drd_rdp_session_post_connect()` - 握手后初始化
- `drd_rdp_session_activate()` - 激活会话并启动渲染线程
- `drd_rdp_session_pump()` - 基础渲染循环（渲染线程启动前的回退）
- `drd_rdp_session_disconnect()` - 断开连接
- `drd_rdp_session_send_server_redirection()` - 发送重定向 PDU（handover）

### DrdRdpGraphicsPipeline

- **头文件**: `drd_rdp_graphics_pipeline.h`
- **职责**: Rdpgfx server 适配器，与客户端交换 CapsAdvertise/CapsConfirm

- **关键方法**:
  - `drd_rdp_graphics_pipeline_maybe_init()` - 尝试初始化管线
  - `drd_rdp_graphics_pipeline_is_ready()` - 检查是否就绪
  - `drd_rdp_graphics_pipeline_wait_for_capacity()` - 等待 ACK 背压
  - `drd_rdp_graphics_pipeline_submit_frame()` - 提交帧
  - `drd_rdpgfx_get_context()` - 获取 FreeRDP Rdpgfx 上下文

## 关键依赖与配置

### 外部依赖

- **FreeRDP**: `freerdp/listener.h`, `freerdp/server/rdpgfx.h`
- **内部依赖**: `core/drd_server_runtime.h`, `utils/drd_encoded_frame.h`

### FreeRDP 回调

会话需要设置以下 FreeRDP 回调：
- `client->Initialize` - 初始化
- `client->PostConnect` - 握手成功
- `client->Activate` - 激活
- `client->Disconnect` - 断开

## 数据模型

### DrdRdpSession 结构

```
DrdRdpSession
├── peer: freerdp_peer*
├── runtime: DrdServerRuntime*
├── graphics_pipeline: DrdRdpGraphicsPipeline*
├── pam_auth: DrdPamAuth*
├── vcm: HANDLE
├── closed_callback: DrdRdpSessionClosedFunc
├── passve_mode: boolean
└── render_running: boolean
```

### DrdRdpGraphicsPipeline 结构

```
DrdRdpGraphicsPipeline
├── peer: freerdp_peer*
├── vcm: HANDLE
├── runtime: DrdServerRuntime*
├── surface_width: uint16
├── surface_height: uint16
├── channel_opened: boolean
├── caps_confirmed: boolean
├── outstanding_frames: uint32
├── max_outstanding_frames: uint32
├── capacity_cond: GCond
└── lock: GMutex
```

## 关键流程

### 会话激活与渲染线程

```
1. 客户端激活 → drd_rdp_session_activate()
2. 调用 drd_server_runtime_prepare_stream() → 启动 capture/input/encoder
3. 启动渲染线程 drd_rdp_session_render_thread()
4. 渲染循环:
   - 等待 capacity_cond (ACK 背压)
   - pull_encoded_frame() → 编码最新帧
   - 提交到 Rdpgfx 或 SurfaceBits
   - 等待下一帧
5. 会话断开 → stop_thread() → drd_server_runtime_stop()
```

### Rdpgfx 背压控制

```
1. renderer 提交帧前调用 wait_for_capacity()
2. 如果 outstanding_frames >= max_outstanding_frames，阻塞等待
3. 客户端发送 FrameAcknowledge → 减少计数 + 唤醒 condition
4. renderer 继续提交下一帧
5. 超时未 ACK → 自动降级到 SurfaceBits
```

### FrameAcknowledge 通知

客户端可能发送 `SUSPEND_FRAME_ACKNOWLEDGEMENT` 标志：
- 立即清空 outstanding_frames = 0
- 广播 capacity_cond
- 下次普通 ACK 恢复背压控制

## 测试与质量

**当前状态**: 无自动化测试

**建议测试方向**：
1. 会话生命周期测试：创建/激活/断开
2. 渲染线程测试：背压控制与降级
3. Rdpgfx 初始化测试：CapsAdvertise/CapsConfirm 握手
4. Handover 测试：Server Redirection 发送

## 常见问题 (FAQ)

### Q: 为何会在激活阶段失败？
A: 可能是客户端能力不足（不支持 DesktopResize）或分辨率不匹配

### Q: 如何调试 Rdpgfx 降级？
A: 查看 `drd_rdp_graphics_pipeline_wait_for_capacity()` 超时日志

### Q: 被动模式有何用？
A: system 模式下，会话仅执行握手不启动渲染，等待 handover 接管

### Q: 渲染线程何时启动？
A: 仅在 `activate()` 成功后启动，确保客户端已准备好接收帧

## 相关文件清单

```
src/session/
├── drd_rdp_session.h            # 会话接口
├── drd_rdp_session.c            # 会话实现
├── drd_rdp_graphics_pipeline.h  # 图形管线接口
└── drd_rdp_graphics_pipeline.c  # 图形管线实现
```

## 变更记录 (Changelog)

### 2025-12-30 - 模块文档初始化

- 新增 `session/CLAUDE.md` 文档
- 记录会话状态机与图形管线
- 记录背压控制与降级机制
- 标记待补充测试
