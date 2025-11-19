## TODO
1. alt 快捷键不生效；
2. 音频下行与麦克风回传；
3. 剪切板共享；
4. 网络探测和流量控制；
5. h264 编码；
6. 硬件加速；
7. 适配lightdm进行远程登录与RedirectPDU保持连接；
   1. lightdm 创建一个虚拟屏幕和远程会话; done
   2. 远程重复登录场景；
   3. 启动lightdm greeter的 drd --handover进程
   4. lightdm conf 配置管理
8. 远程单点登录；
9.  适配wayland(捕获、输入事件、剪切板)；
10. 密钥存储；

- 把drd的启动放到/etc/deepin/greeter.d 目录下，lightdm 启动虚拟屏幕greeter会话时，设置一个环境变量，读到了该环境变量，就启动drd --takeshare 进程，该进程不做端口监听，

- greeter 远程登录：NLA协议；
- 桌面共享：NLA协议；
- 单点登录：TLS协议，拿用户名和密码去做pam认证；
### 单点登录
CredSSP简介

  - CredSSP（Credential Security Support Provider）是 Windows 安全支持提供程序一员，为 RDP、WinRM 等通道提供凭据委派和单点登录能力。
  - 在 RDP 场景中它承担“把客户端用户凭据安全转交给目标主机”的角色，使得终端可以使用本机输入的凭据在远程会话内完成本地/域认证。

  工作流程

  - 客户端首先与服务端通过 TLS 建立受保护信道，TLS 身份验证通常依赖服务器证书。
  - 双方随后利用 SPNEGO/NTLM 或 Kerberos 继续协商安全上下文，客户端验证服务器身份后才会封装并加密凭据。
  - 凭据通过 CredSSP 以安全容器形式发送，服务器端解密后在本地 LSASS 中作为委派凭据使用，实现 RDP 登录与后续单点登录。

  安全特性与风险

  - 依赖 TLS+Kerberos/NTLM 的双层保护，避免中间人窃取。
  - 默认策略仅允许在明确信任的服务器上进行凭据委派，配合 “Restricted Admin” 可限制凭据落地。
  - 历史上存在 CVE-2018-0886 等漏洞，攻击者可在握手阶段降级或执行中间人攻击，需及时打补丁并启用“加密 Oracle 修正级别”。

  使用建议

  1. 强制 RDP 使用 TLS 并部署受信任的服务器证书，防止自签证书绕过。
  2. 在域环境中优先 Kerberos，减少 NTLM 回退。
  3. 启用 CredSSP 更新并设置 Encryption Oracle Remediation=Force Updated Clients，确保客户端/服务器都已修补。
  4. 避免无必要的凭据委派：仅对白名单服务器启用，或采用 “Restricted Admin/Remote Credential Guard” 缓解凭据转储风险。
  5. 结合网络分段与多因素认证进一步降低凭据滥用面。

libfreerdp3-server 与 CredSSP/PAM

  - libfreerdp3-server 只是 FreeRDP 的服务器端框架，用来实现基本 RDP 会话（TLS、RDP 安全层、部分 CredSSP/NLA 握手）和虚拟通道，它本身不会创建或切换 Linux 登录会话，更没有绑定 PAM 的逻
    辑，因此无法直接把客户端凭据交给 PAM 做系统登录或单点登录。
  - 当前 FreeRDP 自带的 freerdp-shadow-cli、wfreerdp-server 等示例只是“共享已有会话”或“把图形回放给客户端”，认证阶段仅验证凭据后放行，不会将拿到的 username/domain/password 写入 PAM，也
    不会启动新的 login session，所以达不到“凭据委派→PAM→SSO”的链路。

  可行性分析

  - CredSSP 由客户端发起，libfreerdp-server 能解析并拿到明文凭据（前提是客户端允许委派），但你需要在 IFreerdpServer::Authenticate/PeerAccepted 之类的回调里手工调用 pam_start/
    pam_authenticate，并依据 PAM 结果决定是否建立 RDP 会话；该功能需要自行编写 glue code，库中没有现成实现。
  - 即便自实现 PAM glue，FreeRDP 服务器目前没有“为成功登录创建独立的 Linux 图形会话并把其输出搬到 RDP”这一基础设施，需要额外结合 Xvnc/Xorg/Wayland compositor 或直接考虑 xrdp（其 sesman
    进程支持 PAM/AD/Kerberos 并创建新会话）。
  - 若只想验证凭据并在上游系统里复用，可将 CredSSP 握手拿到的凭据转成 Kerberos TGT 或调用 LDAP，但这同样需要自定义代码，FreeRDP 官方没有 Remote Credential Guard/Restricted Admin 等高级
    特性。

  建议

  1. 若目标是标准 RDP 服务 + Linux 登录，优先考虑 xrdp（已有 PAM/Kerberos/SAM integration），FreeRDP 更适合做定制场景或网关。
  2. 如果坚持使用 libfreerdp3-server，需要自行实现：CredSSP 握手→提取凭据→PAM 模块→成功后创建会话（Xvnc/Xorg）→把图形输出接回 RDP，工作量相当于重新实现 sesman。
  3. 配合域控时，建议客户端启用 Kerberos/NLA，服务器端确保 TLS 证书可信，避免明文凭据被截获。
  4. 若只需“凭据透传供上游业务使用”而非系统登录，可在服务器端编写 PAM service，把凭据传给内部 API，但注意存储和清理策略，避免密码落地。

协议解析

  - RDP 登录安全协议默认由三层构成：TCP 之上协商 TLS（或 Early RDP Security）提供加密与服务器认证；TLS 隧道内运行 CredSSP（基于 SPNEGO，可选 Kerberos/NTLMSSP/GSS-Negotiate），完成网络级
    身份验证（NLA）；通过 CredSSP 安全地把用户名、密码或智能卡凭据封装并交给会话主机，从而进入图形会话。
  - 在 xrdp 场景，客户端通常启用 EnableCredSSP=1（RDP Security Layer 设为 ssl/negotiate）。握手流程是：1) 客户端发起 RDP Negotiation，声明支持 TLS/NLA；2) xrdp 监听 3389 端口，与客户端
    完成 TLS 握手；3) 在 TLS 隧道内双方启动 CredSSP，客户端用 SPNEGO 选择 Kerberos（域内）或 NTLM（工作组），服务端验证其 TGT/NTLM 响应；4) CredSSP 成功后，客户端将加密的用户名/密码结构
    (TSRemoteGuardPackageCreds) 交给服务端；5) xrdp-sesman 把凭据传入 PAM（pam_unix/pam_systemd 等），为对应 Linux 用户创建 Xvnc/xorgxrdp 会话并加载该用户的桌面。
  - 如果管理员禁用 NLA，xrdp 可以回退到纯 TLS + RDP Security（RC4/128 + RSA 公钥交换），客户端直接把密码用服务器公钥加密后发送，服务端再走 PAM，但该模式易受中间人和凭据重放，生产上建议
    始终启用 CredSSP/NLA 并配合强口令或智能卡。
  - 实现要点：xrdp 侧维护证书与私钥用于 TLS；sesman 通过 PAM/系统d-logind 以登录用户身份启动 X 会话，遵循单一职责（会话调度与认证解耦）；客户端凭据永远在 TLS + CredSSP 双重保护下传输，
    避免明文；可结合 fail2ban、策略限制错误次数提升安全性。

• 客户端在启用 NLA（CredSSP）时，输入的用户名/密码会被前置收集，但绝不是“仅用于网络认证”。CredSSP 在 TLS 隧道内先完成 Kerberos/NTLM 质询响应，从而确认客户端身份并建立受信频道；紧接着
  CredSSP 会把同一份凭据包装成 TSPasswordCreds（或智能卡凭据），发送给服务端的安全子系统。对于 Windows 远程桌面，这些凭据由 LSA/Winlogon 解封后创建会话令牌，直接用来启动对应用户的图形会
  话；对 xrdp 而言，sesman 解出用户名和密码再通过 PAM 调用生成该 Linux 用户的桌面。所以 NLA 的设计目标就是“一次输入 → 同时完成网络级认证与本地会话登录”，避免传统 RDP 那样先空壳连接再弹
  Winlogon。只有在管理员显式禁止凭据委派、改用 Smartcard/SSP 扩展或额外开启凭据二次输入提示时，才会出现“网络认证成功但仍需再次输入密码”的变种。


• 要做到和 xrdp 一样“任意凭据 → PAM”，必须把 CredSSP 的 NTLM/Kerberos 校验链条换成我们自己的 provider，而不是像当前这样仅靠一个静态 SAM 文件“骗过” WinPR。xrdp 的流程是：

  1. 仍然让客户端走 TLS + CredSSP；
  2. 在服务端把 winpr/sspi 中的 AcceptSecurityContext() 重定向到自定义实现（xrdp 的 neutrinordp/credssp/ 模块）；
  3. 自定义实现会解密 TSPasswordCreds，直接把明文账号/口令传给 PAM（或其它后端）做校验；
  4. 如果 PAM 返回成功，就伪造一个 NTLM/Kerberos “已验证”的结果，把成功状态返回给 CredSSP 栈，这样客户端就认为认证通过，CredSSP 后续的数据通路才会打开；
  5. 之后再用同一份凭据启动 sesman / Xorg，实现真正的桌面会话。

  在我们当前的代码里，WinPR 还是默认的 NTLM provider，它一定会去找 SAM 文件算哈希，所以只要客户端输入的账号不等于配置文件里的账号，就会出现 SEC_E_NO_CREDENTIALS。要参考 xrdp 的做法，需
  要做下面这些工作：

  - 拦截或替换 WinPR 的 SSPI 实现：比如在 freerdp_peer 上设置 peer->Authenticate/peer->AuthenticateEx 或直接 patch nla_server_accept()，让 CredSSP 的 AcceptSecurityContext 不再调用
    WinPR 自带的 NTLM，而是调用我们的 PAM 回调。
  - 实现 PAM 驱动的 CredSSP provider：需要读取 CredSSP 流里的 SEC_WINNT_AUTH_IDENTITY / TSPasswordCreds，调用 pam_start/pam_authenticate 判断对错，然后构造一个伪装的 CREDSSP_RESULT 返回
    给客户端。
  - 处理 Kerberos/NTLM 细节：如果只支持用户名+密码，可以像 xrdp 一样只实现 NTLMSSP 的子集；如果要支持域账户，还得处理 Kerberos/SPNEGO。
  - 安全清理：在我们新增的 provider 中，明文密码只能短暂存在，必须在 PAM 认证完成后立即用 SecureZeroMemory 擦除，并确保日志不打印任何凭据。

  这是一项跨 WinPR/Freerdp/PAM 的大改造，而不是在现有代码上加几行逻辑。当前版本还无法像 xrdp 那样接受任意用户凭据，--system 仅仅意味着“不推流，只负责 TLS/NLA + PAM”，但仍然要求客户端输
  入的账号与配置文件保持一致。

  如果你确认要走 xrdp 的路线，就需要：

  1. 在 FreeRDP 源码层面注入自定义 CredSSP provider（可参考 xrdp 的 neutrinordp/credssp 实现或直接移植它的逻辑到 winpr）；
  2. 让 glib-rewrite 链接到这套 provider，并在 delegate 模式下切换为 PAM 校验；
  3. 在计划和文档里明确这是一项大改（涉及安全栈、WinPR、PAM），需要单独评审。


• 在 xrdp 里分两个阶段：

  1. RDP 传输层：xrdp 最初确实是只支持 “TLS/RDP Security Layer + username/password” 的旧模式，客户端把凭据用服务器公钥加密后直接发给 sesman；这时候没有 CredSSP/NLA，安全性取决于 TLS/
     RC4。
  2. NLA/CredSSP：后来 xrdp 又实现了 NLA，客户端仍先走 TLS，但后续就是标准 CredSSP 握手——NTLM/Kerberos 在服务器端验证成功后，客户端把 TSPasswordCreds 下发，xrdp-sesman 再把同一份凭据交
     给 PAM 创建本地会话。所以现在的 xrdp 既能用旧的 TLS-only 模式，也能用 NLA；默认如果客户端开启 NLA，他们会通过 neutrinordp/credssp 模块把凭据拆解下来转给 sesman。

  我们当前的 glib-rewrite 相当于只实现了 xrdp 的第二步里“凭据交给 PAM”这一段，但前面的 NTLM 验证仍靠 WinPR 的 SAM 文件；只要 SAM 里只有固定账号，就会限制客户端必须输入相同的用户名。要达
  到 xrdp 那种真正的“任意凭据 → PAM”，就需要像他们一样自己落地 CredSSP provider 或者直接复用他们的实现，这就是前面提到的那套大改。


你要回退到 “TLS/RDP Security Layer + Server Authenticate 回调” 的老式 RDP 模式，让客户端把用户名/密码直接交给服务器（借 TLS 加密），然后服务器自行做 PAM。
  这几乎等同于 xrdp 初期的做法，不再依赖 CredSSP/NLA。改造要点：

  1. 握手层：在 drd_configure_peer_settings() 里改写安全策略：
      - FreeRDP_TlsSecurity=TRUE，FreeRDP_RdpSecurity=FALSE，FreeRDP_NlaSecurity=FALSE。
      - 还需要配置 FreeRDP_EnableCredSspSupport=FALSE 等，确保客户端不会继续尝试 CredSSP。
      - 记得提前告知管理员：这是退回到 TLS-only 的 RDP 安全，安全级别低于 NLA。
  2. 凭据回调：FreeRDP 为老模式提供了 peer->Authenticate/peer->AuthenticateEx 回调。当客户端在 TLS 层提交用户名/密码时，会调用这个回调；你可以在这里把 char** username/password/domain 拷
     贝出来，然后立刻擦除 FreeRDP 提供的缓冲区（SecureZeroMemory），避免泄漏。
  3. PAM 验证：复用 drd_local_session_new()，用回调里拿到的 username/password 调用 PAM。如果认证成功，就返回 TRUE 让握手继续；失败则返回 FALSE，让客户端收到“认证失败”。
  4. 成功后的会话：通过 DrdRdpSession 把 PAM session 句柄交给后续逻辑；由于没有 CredSSP，这里不会再走 NTLM/SAM，也不存在 rdpSettings->Username 自动填充，需要自己把回调中的账号传下去。
  5. 安全注意：
      - TLS-only 模式下攻击者只要能劫持 TLS，就能重放凭据；没有 CredSSP 的二次保护，也没有 Kerberos/NTLM 的哈希校验，风险更高。
      - 必须确保服务器证书可信、TLS 配置严格（禁用 RC4/SSLv3 等），并在文档中明确该模式的限制和适用场景。
