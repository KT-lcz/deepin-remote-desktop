# core 模块 - 核心层

[根目录](../../CLAUDE.md) > [src](../) > **core**

## 模块职责

core 模块是应用的入口与配置中心，负责：
- 应用启动与命令行解析
- INI/CLI 配置合并
- 运行时聚合（capture/encoding/input）
- TLS 凭据缓存
- 编码选项管理

## 入口与启动

### 主入口

- **文件**: `src/main.c`
- **流程**:
  1. 初始化 WinPR (SSL + WTS API)
  2. 创建 `DrdApplication`
  3. 运行应用 (`drd_application_run`)

### 应用类

- **头文件**: `drd_application.h`
- **接口**:
  - `drd_application_new()` - 创建应用实例
  - `drd_application_run()` - 运行主循环与信号处理

### 配置类

- **头文件**: `drd_config.h`
- **运行模式枚举**:
  - `DRD_RUNTIME_MODE_USER` - 桌面共享
  - `DRD_RUNTIME_MODE_SYSTEM` - 远程登录
  - `DRD_RUNTIME_MODE_HANDOVER` - 连接接管

- **关键接口**:
  - `drd_config_new()` / `drd_config_new_from_file()` - 创建配置
  - `drd_config_merge_cli()` - 合并命令行参数
  - `drd_config_get_*()` - 获取各项配置值

## 对外接口

### DrdServerRuntime

- **头文件**: `drd_server_runtime.h`
- **职责**: 聚合 capture/encoding/input 子系统
- **传输模式**:
  - `DRD_FRAME_TRANSPORT_SURFACE_BITS` - SurfaceBits 回退模式
  - `DRD_FRAME_TRANSPORT_GRAPHICS_PIPELINE` - Rdpgfx 模式

- **关键方法**:
  - `drd_server_runtime_prepare_stream()` - 启动 capture/input/encoder
  - `drd_server_runtime_stop()` - 停止所有子模块
  - `drd_server_runtime_pull_encoded_frame()` - 同步拉取最新帧并编码
  - `drd_server_runtime_set_transport()` - 切换传输模式
  - `drd_server_runtime_request_keyframe()` - 请求强制关键帧

### DrdEncodingOptions

- 编码参数容器
- 通过 `drd_config_get_encoding_options()` 获取
- 传递给 `drd_server_runtime_set_encoding_options()`

## 关键依赖与配置

### 外部依赖

- **GLib**: `glib-2.0`, `gio-2.0`, `gio-unix-2.0`, `gobject-2.0`
- **FreeRDP**: `freerdp-server3`, `freerdp3`, `winpr3`

### 配置文件

项目使用 INI 配置文件 + 命令行覆盖：

```ini
[server]
bind_address=0.0.0.0
port=3390

[tls]
certificate=/usr/share/deepin-remote-desktop/certs/server.crt
private_key=/usr/share/deepin-remote-desktop/certs/server.key

[capture]
width=1920
height=1080
target_fps=60

[encoding]
mode=rfx
enable_diff=true

[auth]
enable_nla=true
username=uos
password=1
pam_service=deepin-remote-sso

[service]
runtime_mode=user
```

## 数据模型

### DrdConfig 结构

```
DrdConfig
├── bind_address: string
├── port: uint16
├── tls_certificate_path: string
├── tls_private_key_path: string
├── nla_username: string
├── nla_password: string
├── enable_nla: boolean
├── runtime_mode: DrdRuntimeMode
├── pam_service: string
├── capture_width: uint
├── capture_height: uint
├── capture_target_fps: uint
└── encoding_options: DrdEncodingOptions
```

### DrdServerRuntime 结构

```
DrdServerRuntime
├── capture: DrdCaptureManager
├── encoder: DrdEncodingManager
├── input: DrdInputDispatcher
├── tls_credentials: DrdTlsCredentials
├── transport_mode: DrdFrameTransport (atomic)
├── stream_running: boolean
└── encoding_options: DrdEncodingOptions
```

## 测试与质量

**当前状态**: 无自动化测试

**建议测试方向**：
1. 配置合并测试：验证 INI + CLI 优先级
2. 运行时模式切换测试：user/system/handover 流程
3. 传输模式原子切换测试：确保线程安全
4. 编码选项传递测试：runtime → encoder 链路

## 常见问题 (FAQ)

### Q: 如何切换运行模式？
A: 通过 INI `[service] runtime_mode=...` 或 CLI `--mode=user|system|handover`

### Q: 如何强制关键帧？
A: 调用 `drd_server_runtime_request_keyframe()`，会将信号传递给 encoder

### Q: 为何编码选项在运行时不可变？
A: 设计上，编码选项在激活时固化，避免热切换导致协商不一致。如需修改需重启会话。

### Q: TLS 凭据何时加载？
A: 仅在启动时加载一次，通过 `drd_tls_credentials_apply()` 注入 FreeRDP settings

## 相关文件清单

```
src/core/
├── drd_application.h          # 应用类接口
├── drd_application.c          # 应用实现
├── drd_config.h               # 配置类接口
├── drd_config.c               # 配置实现
├── drd_server_runtime.h       # 运行时接口
├── drd_server_runtime.c       # 运行时实现
└── drd_encoding_options.h     # 编码选项结构
```

## 变更记录 (Changelog)

### 2025-12-30 - 模块文档初始化

- 新增 `core/CLAUDE.md` 文档
- 记录应用启动流程与配置管理
- 记录运行时聚合模式
- 标记待补充测试