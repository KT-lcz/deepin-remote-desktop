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
- 非交互式嵌入模式可维持默认 `static` 安全策略；若需“一次输入 → CredSSP + PAM 登录”，请在 root/systemd 环境下传入 `--system --nla-mode delegate` 并在配置文件中设置对应的 PAM service（默认 `deepin-remote-desktop-system`），详见 `config/deepin-remote-desktop.service`。
- `--system` 模式仅执行 TLS/NLA 握手与 PAM 会话创建，不会启动 X11 捕获、编码或渲染线程，用于在系统服务里预先创建桌面会话；可选开启 `[service] rdp_sso=true`（或 CLI `--enable-rdp-sso`）让客户端通过 TLS-only RDP 安全层提交凭据，由服务端直接走 PAM 校验。

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
