# RDP 登录安全协议说明

## 上下文
- 用户咨询 xrdp/通用 RDP 登录流程所采用的安全协议及其大致实现方式。
- 需要结合 CredSSP、TLS、NLA 等机制进行分层解释，并联系 Linux 端（xrdp）用户名密码登录。

## 计划
- [x] 梳理 RDP 认证/加密协议栈（TLS → CredSSP → Kerberos/NTLM）以及 xrdp 的默认选型。
- [x] 概述服务端/客户端的实现流程：握手、凭据打包、凭据下推到 PAM、会话初始化等关键步骤。
- [x] 输出“一次输入→双重登录”实施方案并完成落地（CredSSP 委派、PAM 集成、systemd 管理及 `--system` 模式）。

## 进度
- 已完成：整理协议栈并交付两种受支持的登录路径——默认启用 NLA（固定账号 SAM 文件）以及在 `--system` 下关闭 NLA、走 TLS+PAM 单点登录（`[auth] enable_nla=false`/`--disable-nla`）。
- 进行中：等待用户验证 system 部署及多用户登录流程，按需扩展 PAM service 管理。
- 遗留项：依据反馈继续扩展 PAM 配置/用户隔离能力。
