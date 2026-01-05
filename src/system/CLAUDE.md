# system 模块 - 守护进程层

[根目录](../../CLAUDE.md) > [src](../) > **system**

## 模块职责

system 模块负责：
- system 守护进程（远程登录模式）
- handover 守护进程（连接接管模式）
- DBus 服务暴露（org.deepin.RemoteDesktop）
- Routing token 管理与客户端队列
- Server Redirection 协调

## 入口与启动

### System 守护

- **头文件**: `drd_system_daemon.h`
- **创建**: `drd_system_daemon_new(config, runtime, tls_credentials)`
- **启动**: `drd_system_daemon_start(error)`
- **管理**: `drd_system_daemon_set_main_loop(loop)` - 用于优雅退出

### Handover 守护

- **头文件**: `drd_handover_daemon.h`
- **创建**: `drd_handover_daemon_new(config, runtime, tls_credentials)`
- **启动**: `drd_handover_daemon_start(error)`
- **管理**: `drd_handover_daemon_set_main_loop(loop)`

## 对外接口

### DrdSystemDaemon 查询方法

```c
guint drd_system_daemon_get_pending_client_count(DrdSystemDaemon *self);  // 待处理客户端数
guint drd_system_daemon_get_remote_client_count(DrdSystemDaemon *self);   // 总注册客户端数
```

### DrdHandoverDaemon

handover 守护主要通过 DBus 接口与 system 交互，本身不提供额外方法。

## 关键依赖与配置

### 外部依赖

- **GLib**: `glib-2.0`, `gio-2.0`, `gobject-2.0`
- **DBus**: 通过 `gnome.gdbus_codegen` 生成的 `DrdDBusRemoteDesktop`
- **内部依赖**: `core/drd_config.h`, `security/drd_tls_credentials.h`

### DBus 服务配置

- **配置文件**: `data/org.deepin.RemoteDesktop.conf`
- **安装路径**: `/etc/dbus-1/system.d/`
- **服务名称**: `org.deepin.RemoteDesktop`

### systemd 服务

- **system 服务**: `deepin-remote-desktop-system.service`
- **handover 服务**: `deepin-remote-desktop-handover.service`
- **user 服务**: `deepin-remote-desktop-user.service`

## 数据模型

### DrdSystemDaemon 结构

```
DrdSystemDaemon
├── config: DrdConfig*
├── runtime: DrdServerRuntime*
├── tls_credentials: DrdTlsCredentials*
├── main_loop: GMainLoop*
├── bus_name_id: guint
├── remote_clients: GHashTable*  # remote_id -> DrdRemoteClient
├── pending_queue: GQueue*       # 待 handover 客户端
└── current_handover_path: string
```

### DrdRemoteClient 结构（内部）

```
DrdRemoteClient
├── remote_id: string               # /org/deepin/RemoteDesktop/Rdp/Handovers/<token>
├── routing_token: string           # 十进制 token
├── connection: GSocketConnection*
├── session: DrdRdpSession*          # 当前会话（如有）
├── assigned: boolean
├── handover_count: uint
├── username: string
├── password: string
└── last_activity_us: gint64        # 超时检测
```

### Greeter Drop-in

- **文件**: `data/11-deepin-remote-desktop-handover`
- **安装路径**: `/etc/deepin/greeters.d/`
- **功能**: LightDM 启动 greeter 时自动触发 handover

## 关键流程

### System 守护启动流程

```
1. 加载配置与 TLS 凭据
2. 占用 org.deepin.RemoteDesktop DBus 名称
3. 注册 Rdp.Dispatcher 接口
4. 创建 DrdRdpListener（传入 delegate）
5. Listener 启动 → 监听端口
6. 接受连接 → 判断首次/重连
```

### Handover 连接接续流程

```
Phase 1: 初次连接
Client → System (无 token)
  └─ System 生成 token + 注册 DrdRemoteClient
      └─ 响应默认握手请求

Phase 2: Handover 触发
Handover → Dispatcher: RequestHandover()
  └─ System: 返回 handover object path
      └─ Handover → System: StartHandover(username, password)
          └─ System → Handover: certificate PEM + key PEM
              └─ System → Client: Server Redirection PDU (token)
              └─ Client 重连（带 token）

Phase 3: 二次连接与接管
Client → System (带 token)
  └─ System 匹配 token → TakeClientReady()
      └─ Handover → System: TakeClient()
          └─ System: 返回 Unix FD
              └─ Handover: adopt_connection → 继续 CredSSP/会话激活
```

### 队列管理与超时

```
1. 新客户端 → 入 pending_queue（限制 32 个）
2. 定期检查 last_activity_us（超 30 秒踢出）
3. TakeClient 后重置 assigned → 压入队列
4. 下一段 handover 再次 TakeClient → 移除队列
```

### DBus 接口交互

**Rdp.Dispatcher**:
```
RequestHandover() → object_path (handover 对象)

Rdp.Handover:
  StartHandover(username, password) → (cert_pem, key_pem)
  TakeClient() → fd (Unix FD)
  GetSystemCredentials() → (username, password) [待实现]
  RedirectClient(token, username, password) [signal]
  TakeClientReady(use_system_credentials) [signal]
  RestartHandover [signal]
```

## 关键场景

### Greeter 登录 (first handover)

```
1. Client 连接 System → 生成 token
2. LightDM 创建 greeter 会话
3. Greeter handover 启动 → RequestHandover
4. StartHandover → 获取 PEM + 发送 Server Redirection
5. Client 重连 → TakeClient → 推流虚拟屏幕
```

### 用户登录 (second handover)

```
1. Greeter 输入凭据 → LightDM open session
2. User handover 启动 → RequestHandover
3. RestartHandover 信号 → Greeter 手over 发送 Redirect
4. Client 重连 → User handover TakeClient → 推流真实桌面
```

### 断线重入

```
1. 客户端断开 → System 清理 connection
2. 链路保持（DrdRemoteClient 仍存在）
3. 客户端重连（带 token）→ 匹配 → TakeClientReady
4. Handover TakeClient → 重新接管
```

## 测试与质量

**当前状态**: 无自动化测试

**建议测试方向**：
1. DBus 接口测试（RequestHandover/StartHandover/TakeClient）
2. Routing token 生成与匹配测试
3. 队列管理与超时清理测试
4. 多段 handover 流程测试
5. Greeter drop-in 脚本测试

## 常见问题 (FAQ)

### Q: 为何需要 routing token？
A: 跨进程连接的唯一标识符，确保 handover 时客户端被正确路由

### Q: System 与 Handover 如何共享 TLS 凭据？
A: 通过 PEM 字符串传递，系统端调用 `read_material()`，handover 端调用 `reload_from_pem()`

### Q: Pending 队列为何限制 32 个？
A: 防止恶意连接撑爆 DBus 对象与内存

### Q: 如何调试 handover 问题？
A: 查看 System 端 `pending_client_count` 与 `remote_client_count` 指标日志

## 相关文件清单

```
src/system/
├── drd_system_daemon.h       # System 守护接口
├── drd_system_daemon.c       # System 守护实现
└── drd_handover_daemon.h     # Handover 守护接口
```

### 配置与服务文件

```
data/
├── org.deepin.RemoteDesktop.conf        # DBus policy
├── deepin-remote-desktop-system.service # System service
├── deepin-remote-desktop-handover.service # Handover service
├── deepin-remote-desktop-user.service    # User service
├── 11-deepin-remote-desktop-handover     # Greeter drop-in
└── config.d/                              # INI 配置模板
```

## 变更记录 (Changelog)

### 2025-12-30 - 模块文档初始化

- 新增 `system/CLAUDE.md` 文档
- 记录 system/handover 守护与 DBus 服务
- 记录 routing token 管理与队列超时
- 记录多段 handover 与 greeter 登录流程
- 标记待补充测试