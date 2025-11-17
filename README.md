# deepin-remote-desktop (drd)

## 模块划分
- `src/capture`, `src/encoding`, `src/input`, `src/utils`: 组成 `libdrd-media.a`，负责屏幕采集、编码、输入与通用缓冲。
- `src/core`, `src/session`, `src/transport`, `src/security`: 构成 `libdrd-core.a`，封装配置解析、运行时、FreeRDP 监听与 TLS。
- `main.c`: 仅提供入口，链接 `drd-core`（间接包含 `drd-media`）。

## 构建与运行
在 `glib-rewrite/` 内执行：
```bash
sudo apt install meson libpipewire-0.3-dev libsystemd-dev libpolkit-gobject-1-dev libglib2.0-dev freerdp3-dev libx11-dev libxext-dev libxdamage-dev libxfixes-dev libxtst-dev libpam0g-dev
``` 
```bash
meson setup build              # 首次配置
meson compile -C build         # 生成 deepin-remote-desktop 可执行文件
./build/src/deepin-remote-desktop --config ./config/default-user.ini
```
`default.ini` 内置自签名证书 (`certs/server.*`) 及 RemoteFX 配置，可直接用于本地测试。
- 默认启用 NLA：在 `[auth]` 中配置 `username/password` 或使用 `--nla-username/--nla-password`，CredSSP 通过一次性 SAM 文件完成认证，适合单账号嵌入式场景。
- 如需关闭 NLA 并直接复用客户端凭据完成 PAM 登录（单点登录）：在 root/systemd 环境下运行 `--system`，于 `[auth]` 设置 `enable_nla=false`（或 CLI `--disable-nla`）并指定 `pam_service`，客户端 TLS 安全层中的用户名/密码会被转交给 PAM，详见 `config/deepin-remote-desktop.service`。
- `--system` 模式仅执行 TLS/NLA 握手与 PAM 会话创建，不会启动 X11 捕获、编码或渲染线程，用于在系统服务里预先创建桌面会话。

## 样式与工具
- C17 + GLib/GObject，4 空格缩进，类型 `PascalCase`（如 `DrdRdpSession`），函数与变量 `snake_case`。
- 修改 C 文件前可使用 `clang-format -style=LLVM <file>` 对调整区域进行格式化。
- 日志输出统一使用英文（`g_message`, `g_warning` 等）。

## 目录导航
- `doc/`: 架构（`architecture.md`）与变更记录（`changelog.md`）。
- `config/`: 示例配置；可复制并修改证书、编码、采集参数。
- `.codex/plan/`: 当前任务看板（`glib-rewrite.md`）以及 rebrand 计划。

## 提交建议
- Commit 信息延续仓库惯例：简洁中文标题 + 需要时附说明。
- 推送前请在 README 中列出的命令下做最小验证（构建 + 启动一次服务）。

## 目前支持的身份验证路径：

1. `enable_nla=true`（默认）：通过 CredSSP + 一次性 SAM 文件校验固定账号，适用于单账号嵌入式/桌面注入场景。
2. `enable_nla=false` + `--system`：切换到 TLS-only RDP Security，`drd_rdp_listener_authenticate_tls_login()` 读取客户端提交的用户名/密码并交给 PAM，完成“客户端凭据 → PAM 会话”的单点登录。

• 若未来希望“保留 NLA，同时接受任意用户名/密码”，需要参考 xrdp 的 CredSSP provider 方案，投入一次跨 WinPR/Freerdp/PAM 的大改造：

- 拦截或替换 WinPR 的 SSPI provider，让 `AcceptSecurityContext()` 不再依赖 SAM，而是调用自定义的 PAM 驱动逻辑。
- 在自定义 CredSSP provider 中解析 `SEC_WINNT_AUTH_IDENTITY/TSPasswordCreds`，调用 `pam_start/pam_authenticate` 校验账号，成功后伪造 NTLM/Kerberos 成功结果返回给 CredSSP。
- 处理 NTLM/Kerberos 兼容性（最小实现可以只支持 NTLMSSP，若要兼容域账号需补齐 SPNEGO/Kerberos）。
- 明文密码必须在 PAM 操作后立即用 `SecureZeroMemory` 擦除，并保证日志永不打印敏感字段。
- 文档/计划需同步声明该模式的风险、部署前提与回退方案。

• TLS-only 模式已经内建，无需再实现 `peer->Authenticate` 回调。进一步的改进方向：

- 更严格的 TLS 策略（证书轮换、禁用弱密码套件、OCSP）。
- PAM service 的多因素扩展与账号隔离策略。
- 更完善的日志审计：记录每次 TLS/PAM 登录的来源、会话寿命与清理状态。

