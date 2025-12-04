# deepin-remote-desktop (drd)

## 模块划分
- `src/capture`, `src/encoding`, `src/input`, `src/utils`: 提供采集、编码、输入与通用缓冲实现。
- `src/core`, `src/session`, `src/transport`, `src/security`: 负责配置解析、运行时、FreeRDP 监听与 TLS。
- `main.c`: 应用入口。Meson 直接将上述源文件编译成单一的 `deepin-remote-desktop` 可执行文件，不再生成中间静态库。

## 构建与运行

### 依赖
需要 Meson ≥ 0.60、Ninja、pkg-config，以及 GLib/FreeRDP/X11/PAM 头文件。在 Debian/Ubuntu 上可一次性安装：

```bash
sudo apt install meson ninja-build pkg-config libsystemd-dev libpolkit-gobject-1-dev \
libglib2.0-dev libpam0g-dev libx11-dev libxext-dev libxdamage-dev libxfixes-dev libxtst-dev\
freerdp3-dev libwinpr3-dev
```

### 构建/验证
```bash
meson setup build --prefix=/usr --buildtype=debugoptimized  # 首次配置
meson compile -C build                                      # 生成可执行文件
meson test -C build --suite unit                           # 可选：运行单元测试
./build/src/deepin-remote-desktop --config ./config/default-user.ini
```

`config.d` 中提供了 NLA 固定账号、systemd handover、PAM system 模式等示例；`data/certs/server.*` 则内置了一套开发用 TLS 证书，可直接 smoke。

- 默认启用 NLA：在 `[auth]` 中配置 `username/password` 或使用 `--nla-username/--nla-password`，CredSSP 通过一次性 SAM 文件完成认证，适合单账号嵌入式场景。
- `enable_nla=false` + `--system`：切换到 TLS-only + PAM 登录，客户端凭据会在 system 模式下交给 PAM，适合桌面 SSO。
- `--system` 模式仅执行 TLS/NLA 握手与 PAM 会话创建，不会启动 X11 捕获、编码或渲染线程，真正的图像/输入在 handover 阶段启动。

### 安装
`meson install` 会把配置模板、certs 开发证书集以及 systemd/greeter drop-in 安装至 `/usr/share/deepin-remote-desktop/`、`/usr/lib/systemd/{system,user}` 和 `/etc/deepin/greeters.d/`。若要打包或预览，可执行：

```bash
meson install -C build --destdir="${PWD}/pkgdir"
tree pkgdir
```

可执行文件可直接从 `./build/src/deepin-remote-desktop` 运行，或交由 systemd unit 托管。

### Debian 打包
仓库自带 `debian/` 目录（debhelper + Meson）。默认规则将：

1. 以 `prefix=/usr` 配置 Meson，并开启 `DEB_BUILD_MAINT_OPTIONS=hardening=+all`;
2. 调用 `meson install --destdir=debian/tmp` 并手动复制 `deepin-remote-desktop` 主程序；
3. 收集 `/usr/share/deepin-remote-desktop/config.d`、greeter handover 脚本以及 system/systemd user unit。

构建命令：

```bash
sudo apt install devscripts debhelper
dpkg-buildpackage -us -uc
```

生成的 `.deb` 位于仓库上层，例如 `../deepin-remote-desktop_0.1.0-1_amd64.deb`。安装后可通过 `systemctl start deepin-remote-desktop-system.service` 或 `systemctl --user start deepin-remote-desktop-user.service` 进入实际部署。

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

## 调试
1. drd 日志: export G_MESSAGES_DEBUG=all
2. freerdp 日志: export WLOG_LEVEL=debug