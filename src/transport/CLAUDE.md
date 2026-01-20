# transport 模块 - 传输层

[根目录](../../CLAUDE.md) > [src](../) > **transport**

## 模块职责

transport 模块负责：
- RDP 监听器（继承 GSocketService）
- 接受新连接并创建 FreeRDP peer
- Routing Token 提取与匹配
- TLS/NLA 配置注入
- System/handover 连接托管

## 入口与启动

### 监听器创建

- **头文件**: `drd_rdp_listener.h`
- **创建**: `drd_rdp_listener_new()`

**参数**:
- `bind_address`: 绑定地址（如 `0.0.0.0`）
- `port`: 监听端口（如 `3390`）
- `runtime`: 运行时实例
- `encoding_options`: 编码选项
- `nla_enabled`: 是否开启 NLA
- `nla_username/nla_password`: NLA 凭据
- `pam_service`: PAM service 名称
- `runtime_mode`: 运行模式（user/system/handover）

### 监听器控制

- `drd_rdp_listener_start()` - 启动 GLib GSocketService
- `drd_rdp_listener_stop()` - 停止监听
- `drd_rdp_listener_adopt_connection()` - 手动接管连接（handover）

## 对外接口

### 代理机制 (Delegate)

允许外部处理新连接（用于 system handover）：

```c
typedef gboolean (*DrdRdpListenerDelegateFunc)(
    DrdRdpListener *listener,
    GSocketConnection *connection,
    gpointer user_data,
    GError **error
);
```

调用 `drd_rdp_listener_set_delegate()` 注册代理：
- 代理返回 TRUE → 连接已处理，跳过默认逻辑
- 代理返回 FALSE → 继续默认创建 peer

### 会话回调

```c
typedef gboolean (*DrdRdpListenerSessionFunc)(
    DrdRdpListener *listener,
    DrdRdpSession *session,
    gpointer user_data
);
```

调用 `drd_rdp_listener_set_session_callback()` 注册：
- 认证完成的 PostConnect 阶段触发
- 会话携带 system 侧元数据（如 drd-system-client、drd-system-keep-open）

### 辅助方法

- `drd_rdp_listener_get_runtime()` - 获取关联运行时
- `drd_rdp_listener_is_handover_mode()` - 检查是否为 handover 模式

## 关键依赖与配置

### 外部依赖

- **GLib**: `gio/gio.h`
- **内部依赖**: `core/drd_config.h`

### Routing Token

- **头文件**: `drd_rdp_routing_token.h`
- **功能**: 使用 MSG_PEEK 从 TPKT/x224 中提取 `Cookie: msts=<token>`
- **接口**:
  - `drd_routing_token_peek()` - 提取 token
  - `drd_routing_token_generate()` - 生成新 token（用于首次连接）

### Token 格式

```
Cookie: msts=123456\r\n
```

或通过 Server Redirection PDU 重定向时携带完整的 routing token。

## 数据模型

### DrdRdpListener 结构

```
DrdRdpListener (继承 GSocketService)
├── bind_address: string
├── port: uint16
├── runtime: DrdServerRuntime*
├── encoding_options: DrdEncodingOptions*
├── nla_enabled: boolean
├── nla_username: string
├── nla_password: string
├── pam_service: string
├── runtime_mode: DrdRuntimeMode
├── sessions: GPtrArray*
├── delegate: DrdRdpListenerDelegateFunc
├── session_callback: DrdRdpListenerSessionFunc
└── delegate_user_data: gpointer
```

### Routing Token 提取流程

```
1. GSocketConnection 创建
2. MSG_PEEK 读取 TPKT header (3 bytes)
3. 判断是否携带 Cookie: msts=
4. 如有 → 解析 token → 返回
5. 如无 → 返回 NULL（首次连接，需生成 token）
```

## 关键流程

### 默认连接处理（user 模式）

```
1. GLib 触发 incoming() 信号
2. 检查 delegate 是否处理
3. 复制 socket fd
4. 创建 freerdp_peer
5. 注入 TLS/NLA 配置
6. 创建 DrdRdpSession
7. 注册会话到 sessions 列表
8. 启动 FreeRDP 事件线程
```

### Handover 连接处理（system 模式）

```
1. GLib 触发 incoming()
2. drd_system_daemon_delegate() 拦截
3. peek routing token
4. 如匹配 → 创建 DrdRemoteClient → 导出 DBus 对象
5. handover 端调用 RequestHandover() → StartHandover()
6. system 端发送 Server Redirection PDU
7. 客户端重连（带 token）
8. system 端再次 incoming() → token 匹配 → TakeClientReady
9. handover 端调用 TakeClient() → 获取 fd
10. handover 监听器 adopt_connection() → 继续握手
```

### FD 复制与生命周期

```
GSocketConnection → dup(fd) → freerdp_peer
↓
GLib 连接关闭
↓
FreeRDP 管理生命周期
↓
Disconnect → session_closed() → g_ptr_array_remove()
```

## 测试与质量

**当前状态**: 无自动化测试

**建议测试方向**：
1. 监听器启动与绑定测试
2. 连接接受与 peer 创建测试
3. Routing token 提取测试（首次/重连）
4. Delegate 机制测试（system 模式）
5. 会话列表管理测试

## 常见问题 (FAQ)

### Q: 为何监听器继承 GSocketService？
A: 利用 GLib 主循环驱动监听，避免手动事件循环

### Q: Routing token 如何保证唯一？
A: system 模式下生成十进制 token，与 remote_id 互逆映射

### Q: Handover 时为何不会创建重复 peer？
A: listener 检测到 delegate 已处理会提前返回，避免重复创建

### Q: 如何调试连接问题？
A: 开启 `WLOG_LEVEL=debug` 查看 FreeRDP 握手日志

## 相关文件清单

```
src/transport/
├── drd_rdp_listener.h          # 监听器接口
├── drd_rdp_listener.c          # 监听器实现
├── drd_rdp_routing_token.h     # Routing token 接口
└── drd_rdp_routing_token.c     # Routing token 实现
```

## 变更记录 (Changelog)

### 2025-12-30 - 模块文档初始化

- 新增 `transport/CLAUDE.md` 文档
- 记录 GSocketService 监听器与 FreeRDP peer 创建
- 记录 routing token 提取与 handover 流程
- 标记待补充测试
