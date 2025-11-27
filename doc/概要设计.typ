#import "@preview/fletcher:0.5.8" as  fletcher: diagram, node, edge, shapes

#set text(
    font: (
      "LXGW WenKai Mono",
      "Noto Sans Mono CJK SC",
      "Noto Sans Mono",
    )
)

#set heading(numbering: "1.1.1.")

#set page(header:
  align(right)[概要设计说明书]
)

#show raw.where(block: false): box.with(
    fill: luma(240),
    inset: (x: 3pt, y: 0pt),
    outset: (y: 3pt),
    radius: 2pt,
)

#show raw.where(block: true): block.with(
    width: 100%,
    fill: luma(240),
    inset: 10pt,
    radius: 4pt,
)

#figure(
  align(center)[#table(
    columns: (auto, 40%, auto, auto),
    align: (center,center,center,center,center,),
    table.header(table.cell(align: left, colspan: 4)[],),
    table.hline(),
    table.cell(align: center, colspan: 4)[#strong[deepin-remote-desktop 概要设计说明书];],
    table.cell(align: center, colspan: 4)[],
    table.cell(align: center)[#strong[配置项编号];], table.cell(align: center)[DRD-OV-2025], table.cell(align: center)[#strong[版本号];], table.cell(align: center)[V1.0],
    table.cell(align: center)[#strong[文档密级];], table.cell(align: center)[内部], table.cell(align: center)[#strong[编制部门];], table.cell(align: center)[deepin remote desktop 团队],
    table.cell(align: center)[#strong[编制人];], table.cell(align: center)[Codex], table.cell(align: center)[#strong[日期];], table.cell(align: center)[2025年11月26日],
    table.cell(align: center)[#strong[审核人];], table.cell(align: center)[待定], table.cell(align: center)[#strong[日期];], table.cell(align: center)[待定],
    table.cell(align: center)[#strong[批准人];], table.cell(align: center)[待定], table.cell(align: center)[#strong[日期];], table.cell(align: center)[待定],
  )]
  , kind: table
)

修订记录

#figure(
  align(center)[#table(
    columns: (auto, 10.26%, 30.96%, 15.66%, 11.34%, 11.34%, 11.47%),
    align: (center,left,left,center,center,center,center,),
    table.header(table.cell(align: center)[#strong[序号];], table.cell(align: center)[#strong[版本号];], table.cell(align: center)[#strong[修订内容描述];], table.cell(align: center)[#strong[修订日期];], table.cell(align: center)[#strong[修订人];], table.cell(align: center)[#strong[审核人];], table.cell(align: center)[#strong[批准人];],),
    table.hline(),
    table.cell(align: center)[1], table.cell(align: left)[V1.0], table.cell(align: left)[首版：转换为 typst 模板，补充 LightDM 集成说明、系统/媒体/安全设计。], table.cell(align: center)[2025-11-26], table.cell(align: center)[Codex], table.cell(align: center)[待定], table.cell(align: center)[待定],
    table.cell(align: center)[], table.cell(align: left)[], table.cell(align: left)[], table.cell(align: center)[], table.cell(align: center)[], table.cell(align: center)[], table.cell(align: center)[],
    table.cell(align: center)[], table.cell(align: left)[], table.cell(align: left)[], table.cell(align: center)[], table.cell(align: center)[], table.cell(align: center)[], table.cell(align: center)[],
    table.cell(align: center)[], table.cell(align: left)[], table.cell(align: left)[], table.cell(align: center)[], table.cell(align: center)[], table.cell(align: center)[], table.cell(align: center)[],
    table.cell(align: center)[], table.cell(align: left)[], table.cell(align: left)[], table.cell(align: center)[], table.cell(align: center)[], table.cell(align: center)[], table.cell(align: center)[],
  )]
  , kind: table
)

#pagebreak()
#outline(title: "目录")

#pagebreak()
= 概述
== 目的
deepin-remote-desktop（简称 drd）提供 Linux 上的现代 RDP 服务端，实现远程登录、桌面共享。本文件面向熟悉 GLib/FreeRDP 的工程师，说明系统设计原则、模块边界、关键流程及 LightDM(displayManager) 扩展（代码位于 upstream/lightdm-rdp2），为后续详细设计、测试与部署提供统一基线。

== 术语说明
- RDP：Remote Desktop Protocol，本项目基于 FreeRDP 3.x 服务器栈。
- System/Handover：远程登录两段式守护，System 负责集中监听与 LightDM/GDM 对接，Handover 负责 greeter 与用户会话接力。
- Runtime：DrdServerRuntime，串联 capture、encoding、input 的媒体流水线。
- Rdpgfx：Graphics Pipeline 虚拟通道，用于 Progressive/RFX 帧推送。
- NLA/CredSSP：网络级认证，控制凭据交付方式。
- LightDM SeatRDP：lightdm 中的远程 seat 实现。
- Routing Token：Server Redirection 使用的 msts cookie，用于系统守护识别重连上下文。

== 参考资料
1. FreeRDP 3.x API 文档;
2. LightDM Seat 与 DisplayManager 接口;
3. deepin systemd/DBus 规范。

= 系统设计
== 设计原则
- 使用c基于glib进行开发。
- rdp协议使用freerdp3库实现。
- 遵循 deepin/UOS systemd/DBus 规范。
- 遵循 SOLID：监听、会话、媒体、系统守护、LightDM Seat 均以单一职责交付，抽象层稳定，具体实现可替换。
- KISS：运行时仅保留必要线程（X11 capture + renderer），避免额外 dispatcher 或深层回调链。
- DRY：帧数据结构、日志、凭据加载与 DBus 交互集中复用，lightdm 模块共用 RemoteDisplayFactory/SeatRDP，杜绝重复脚本。
- YAGNI：未落地的 H.264、Wayland、虚拟通道等仅记录在 TODO 与本文档规划，不预先引入空壳模块。
- 安全默认开启：TLS + NLA，System/Handover 重定向链路加密。

== 需求分析
本系统主要解决以下关键需求：

1. 远程桌面功能需求
- 支持X11下的桌面共享
- 支持X11下的远程greeter登录
- 支持X11下的远程单点登录

2. 技术目标
- 支持主流RDP客户端连接
- 支持多客户端同时连接
- 资源占用(CPU/内存)控制在合理范围内

3. 使用场景分析
- TODO

4. 技术方案概述
- 使用XShm和XDamage进行屏幕捕获；
- 使用 Rdpgfx Progressive 与 SurfaceBits 进行编码和传输；
- 通过拆分帧为64x64的网格进行比对，只更新变化的部分降低带宽占用；
- 使用xserver-xorg-video-dummy实现虚拟屏幕创建；
- 使用lightdm创建远程会话和单点登录会话；
- 使用pam进行单点登录认证；
- 采用RDP server redirect的方案保证远程登录过程中客户端不断连以及远程会话可以重复使用；
- 使用XTest的方式模拟键鼠输入事件；

== 主要模块
本系统主要包含两大模块：deepin-remote-desktop 总计 10 个子模块；lightdm 总计 2 个子模块。

deepin-remote-desktop的进程之间通过DBus通讯，deepin-remote-desktop和lightdm之间通过DBus通讯，桌面UI组件和deepin-remote-desktop之间通过DBus通讯，lightdm父子进程之间通过管道通讯，deepin-remote-desktop监听例如3389网络端口和RDP客户端通讯，系统整体结构如下:
#image("uml/deepin-remote-structure.png")
#pagebreak(weak: true)
核心模块之间交互如下:
#image("uml/deepin-remote-desktop.png")

- drd-system 为deepin-remote-desktop --mode system进程
- drd-handover 为deepin-remote-desktop --mode handover进程
- 远程登录依赖需要drd-system 和 drd-handover实现；
- drd-user 为deepin-remote-desktop --mode user进程，桌面共享需要drd-user依赖实现；
- 远程登录和桌面共享在客户端连接管理、屏幕采集管理、编码与传输管理、输入事件管理、连接安全管理、配置与隐私管理模块上是复用的；但是运行时除端口号外，两者之间不涉及业务交叉。
=== 客户端连接管理
*作用*：
- 监听 RDP 客户端连接、和客户端完成协商与握手；
- 对连接进行暂停、终止等管理；

*机制*：
- 通过 GSocketService 监听端口，注册incoming回调函数；
- 通过注册 freerdp_peer 回调函数完成freerdp 配置、会话、上下文的管理；
- 校验 routing token 以决定是否需要重定向本次连接；


=== 屏幕采集管理
*作用*：
- 持续采集桌面图像，向编码器提供统一帧源。

*机制*：
- 在独立线程订阅 XDamage 事件，并借助 XShm 共享内存复制像素，再以单帧阻塞队列交付最新画面。

=== 编码与传输管理
*作用*：
- 选择合适的编码格式并通过 Rdpgfx 或 SurfaceBits 推流，兼顾画质与稳定性。

*机制*：
- 基于客户端协商启用 RemoteFX Progressive（RLGR1）或 Raw，启动RDP Graphics Pipeline的虚拟通道，进行编码帧传输。

=== 输入事件管理
*作用*：
- 将远程键鼠操作注入本地桌面，保持交互一致。

*机制*：
- 通过 FreeRDP 输入回调统一缩放坐标、剥离 0xE0 扩展位，再用 XTest 将事件注入 X11。

=== 连接安全管理
*作用*：
- 提供 TLS/NLA/PAM 的凭据防线，确保所有连接都在安全的信道中完成认证。

*机制*：
- 禁用纯 RDP Security；
- 桌面共享支持使用密码连接和非密码连接，密码连接时使用NLA+TLS方案，密码为NLA密码；非密码连接使用TLS+用户确认的方式；
- 远程greeter登录仅支持使用密码连接，使用NLA+TLS方案，密码为NLA密码，客户端显示服务端greeter界面后，由用户输入服务端账户密码完成登录；
- 远程单点登录仅支持使用密码连接，使用TLS+PAM方案，用户客户端配置的密码为服务端账户和密码，drd-system进程获取到账户和密码后进行pam_sso认证，认证通过后向lightdm请求开启对应账户的桌面会话；
- 自动生成TLS证书和指纹，支持自动轮换；
- 支持自动生成NLA强密码；
- NLA密码存储在可信路径或设备中；
- 服务重定向过程中使用随机密码和token；
``` TODO
  Handover一次性凭据的随机化

  - handover 模式下，GrdCredentialsOneTime 在构造阶段直接从/dev/urandom读取 16 字节，转换
    为仅包含可打印 UTF‑8 的用户名/密码（必要时把#、:替换为_），然后将这对凭据缓存于内存而
    不落盘（gnome-remote-desktop-48.0/src/grd-credentials-one-time.c:45-204）。
  - 触发 StartHandover 时，handover 进程把这对一次性凭据传给 system 守护，后者再打包到
    Server Redirection PDU 中，让客户端在重连后用该凭据重新进行 NLA 认证（gnome-remote-
    desktop-analyize/11-NLA安全协议实现机制.md:18-58、gnome-remote-desktop-analyize/02-远
    程登录实现流程与机制.md:52-66）。
  - 设计动机：① handover 进程本身不掌握真实系统账户，降低凭据泄露风险；② 每次重连都强制
    刷新口令，客户端即使截获也只能使用一次，符合最小权限和 YAGNI（只提供当前 handover 所
    需）；③ system 可在 use_system_credentials 为真时再单独下发长期口令，保持与 Credential
    Store 的解耦。若不做随机化，需要存储或传递真实用户密码，既违反最小暴露原则，也增加凭据
    生命周期管理复杂度。

  Routing Token 随机生成的必要性

  - 每个新连接都会调用 get_next_available_id()，其中通过 g_random_int() 抽取 32 位随机值作
    为 routing token，并据此构造 D-Bus handover object path（/org/gnome/RemoteDesktop/Rdp/
    Handovers/session<token>），只有未在 remote_clients 哈希表中存在时才使用，确保唯一性
    （gnome-remote-desktop-48.0/src/grd-daemon-system.c:536-555）。
  - routing token 被发送到客户端的 Server Redirection PDU 中；客户端在每次重连时必须携
    带它，system 才能在 on_incoming_redirected_connection() 中恢复对应的 GrdRemoteClient
    并触发 TakeClientReady（gnome-remote-desktop-48.0/src/grd-daemon-system.c:605-653、
    gnome-remote-desktop-analyize/02-远程登录实现流程与机制.md:24-135）。
  - 随机化的原因：① token 是“能力票据”，拥有它就能把 system 中的等待连接绑定到任意
    handover；若可预测或重复，其他客户端可能窃取会话；② handover 要求同一个 token 支持多
    次重连（system→greeter→user 以及后续断线恢复），使用随机值可避免对象路径冲突并简化状
    态表查找（单一键即可定位所有上下文）；③ token 也作为日志/调试标识，不重复有助于追踪具
    体链路。综上，随机 token 提供了安全隔离与稳定的多阶段路由能力，是 handover 流程中保持
    KISS（单键索引）和 SOLID（system 通过抽象 token 与 handover 解耦）的关键。

```
=== 服务重定向管理
*作用*：
- 串联从greeter到用户会话全过程，实现整个链路中用户无感知的切换RDP服务端。

*机制*：
- 在greeter会话和用户会话分别启动drd-handover进程用于捕获屏幕和传输，基于rdp的server redirect协议，让客户端进行用户无感知的自动重连；
- 在重连时，将RDP服务端进行切换；典型场景：由greeter的drd-handover进程切换到用户会话的drd-handover进程；
- 使用一个system进程drd-system监听端口，客户端第一次连接时，使用drd-handover的密码和随机生成的routing_token包装进server redirect PDU中发给客户端，让客户端重连，客户端第二次重连后，drd-system进程不再和客户端进行握手与协商，而是将accept返回的fd通过dbus接口转交给drd-handover进程，由drd-handover进程进行握手与协商，后续由handover进程进行屏幕捕获和传输；


=== 远程会话管理
*作用*：
- 实现同时可接收多个客户端的连接请求；
- 实现客户端可以重复连接一个远程会话；

*机制*：
- 通过维护 routing token、remote_id 以及session_id，避免连接错配；
- 每次客户端连接时，drd-system调用lightdm创建远程会话；
- lightdm的remote session需要保存remoteid(也就是routing token)和session id；
- 然后drd-system都会创建一个handvoer dbus object导出，path为/org/deepin/remoteDesktop/RDP/Handovers/session+\${session_id}，并且持有remoteid；
- drd-handover进程请求RequestHandover时,根据drd-handover进程的session_id将对应objectpath返回；
- 当用户登录之后，lightdm的remote session的session id会发生变化，但是remoteid不会，会创建一个新的handover dbus object1导出，remoteid和刚才的一致，但是sessionid是新的，用户会话的drd-handover进程请求 RequestHandover 时，找到对应sessionid的dbus object1，用户会话的drd-handover进程请求starthandover时，找到和object1 remoteid相同的object0，由object0发出redirectclient信号，greeter handover 发送redirectPDU并退出。object1再次接收到客户端连接时，再将fd交给用户会话的drd-handover进程。
- 

- 如果登录的是已经存在远程会话的账户时，同样会发生session id的变化，但是
- 


*远程会话重入场景：*
- 已经存在用户会话的handover进程，监听已经存在的handover object信号；
- 
- lightdm已经存在一个remote session,remoteid为上一个客户端连接的remoteid,sessionid为当前会话的id；system的handover object监听这个session的prop changed信号；
- 客户端连接时，lightdm会创建一个新的remote session,remoteid为新的remoteid,sessionid为greeter的sessionid；greeter handover进程接管连接，用户完成认证之后，lightdm发现该用户已经存在了一个remote session，那么就更新原有的remote session的remoteid,然后释放新建的remote session，不再开启新的用户会话。
- 更新原有session的remoteid后，system进程的响应该changed信号，也更新自身的remoteid,然后发出restarthandover信号，用户会话的handover进程重新调用该object的starthandover,然后system进程根据新的remoteid,找到本次连接新创建的object,发redirect信号，greeter handover发redirectpdu,然后关闭连接，由用户会话的handover接管本次连接，传输已经存在的remote session的桌面。

// === 多会话管理
// *作用*：
// 维护 routing token、remote_id 与 handover 状态，避免连接错配。

// *机制*：
// 通过哈希表与 pending 队列记录 remote_id ↔ token 映射，并依靠 handover_count/assigned 字段调度 TakeClient 顺序，同时在 DBus 属性中公开状态。

// *实现*：
// 目前仅实现 O(1) token 命中与两段 handover 重排，过期清理与优先级策略仍在路线上。

=== 远程会话权限控制
*作用*：
划定 system、greeter、用户会话之间的认证边界，避免凭据越权。

*机制*：
启用 NLA 时要求客户端提供配置账号并通过一次性 SAM 文件限制作用域；禁用 NLA 时改走 TLS+PAM，并借助 REMOTE_HOST/PAM_RHOST 为 logind 提供审计信息，同时 DBus 层用 use_system_credentials 控制系统凭据访问。

*实现*：
目前仅验证静态 NLA 账号和 TLS+PAM，两者都以方案验证为目的，未来会同 LightDM SSO、外部策略引擎结合。

=== 配置与隐私管理
*作用*：
集中管理 capture、encoding、auth、security、service 等配置，并保护敏感字段。

*机制*：
合并 CLI/INI，校验证书路径、账号等参数，并在日志中用 \*\* 掩码隐藏敏感内容。

*实现*：
现阶段只支持静态加载与基本校验，用来验证“单一配置视图 + 日志脱敏”，热加载和分层覆盖将在正式版实现。

=== 进程管理
*作用*：
协调 user/system/handover 三种模式与 systemd 的关系。

*机制*：
通过 DrdRuntimeMode 决定启动媒体 runtime、system daemon 或 handover 守护，并以 GMainLoop 作为主循环供 systemd 控制。

*实现*：
当前仅验证“单二进制 + 三态模式”能与 systemd 协同，watchdog、健康检查与分布式调度仍在规划。

=== UI 模块管理（greeter 控制中心 任务栏）
*作用*：
为用户展示远程连接状态并提供操作入口。

*机制*：
LightDM 通过 RemoteDisplayFactory/SeatRDP 启动远程 greeter，remote-server 将 handover object path 透传给 system 守护；控制中心与任务栏订阅 org.deepin.RemoteDesktop 信号显示状态或发起断开。

*实现*：
UI 仍处于方案验证阶段，仅保证 DBus 信号与 LightDM 回调链畅通，历史记录与权限审计等待后续版本实现。
== 关键流程
=== 桌面共享流程
1. `DrdApplication` 解析配置并初始化日志/TLS，创建 `DrdServerRuntime`，在 `prepare_stream()` 中顺序启动 capture/input/encoding。
2. `DrdRdpListener` 在 `incoming` 中创建 `freerdp_peer`，完成 CredSSP/TLS 握手；PostConnect 时 renderer 线程启动，循环执行“等待 Rdpgfx 容量 → pull_encoded_frame → 编码 → 提交”。
3. Progressive 失败或客户端不支持 Rdpgfx 时，runtime 将 transport 切换为 Raw，SurfaceBits 逐行拆包并发送；恢复后 session 请求关键帧以回到 Progressive。
4. 输入通过 FreeRDP 回调进入 `drd_input_dispatcher`，由 XTest 注入真实桌面；会话关闭时 runtime/输入/编码按序释放，下一次共享保持干净状态。

=== 远程 greeter 登录
1. system 守护监听 3389，若首包缺少 routing token 会随机生成并缓存 `DrdRemoteClient`，随后通过 LightDM RemoteDisplayFactory 创建 `SeatRDP` 与 remote-server。
2. Greeter handover 向 Dispatcher 请求对象，调用 `StartHandover` 后获取一次性凭据与 TLS PEM，system 守护向客户端发送 Server Redirection（目标为 greeter）。
3. 客户端携 token 重连 system，delegate 识别 token 并向 greeter handover 发送 `TakeClientReady`；handover 通过 `TakeClient` 获取 socket fd，复用本地 listener 继续 CredSSP，LightDM greeter 展示登录界面。
4. remote-server 通过 DBus 同步会话状态，控制中心/任务栏据此提示“正在远程登录”并允许中止。

=== 远程单点登录
1. 用户在 greeter 认证成功后，LightDM 触发用户 handover；system 守护收到 `StartHandover` 后向 greeter handover 发出 `RedirectClient`，要求其向客户端发送第二次 Server Redirection。
2. 客户端断开 greeter handover 并携既有 routing token 重连 system，delegate 将连接交付用户 handover；handover 以普通用户身份运行，沿用 system 侧下发的 TLS/NLA 设置。
3. 若启用系统级 SSO，`GetSystemCredentials` 会返回一次性凭据；当前实现默认沿用客户端输入的帐密，通过 CredSSP 完成认证，后续可在 LightDM/D-Bus 中植入 SSO 接口。
4. 用户会话激活后 renderer 继续推流，`drd_rdp_session_send_server_redirection()` 状态记录日志；手动断开或异常退出都会触发控制中心/任务栏更新，保证远程单点登录全程可追踪。

#align(
  center,
  figure(
    caption: [System→Greeter→用户时序],
    diagram(
      node-stroke: 0.6pt,
      node-fill: white,
      node-corner-radius: 4pt,
      let (
        s1,
        s2,
        s3,
        s4,
        s5,
        s6,
        s7
      ) = (
        (0, 6),
        (0, 5),
        (0, 4),
        (0, 3),
        (0, 2),
        (0, 1),
        (0, 0)
      ),
      node(s1, "Client → System\\n初次建立 TLS/NLA"),
      node(s2, "System → LightDM\\n请求远程 Seat"),
      node(s3, "Greeter StartHandover\\nSystem 发送 Server Redirection"),
      node(s4, "Client 重连\\nGreeter TakeClient"),
      node(s5, "User StartHandover\\nSystem 通知 Greeter Redirect"),
      node(s6, "Client 再次重连\\nUser TakeClient"),
      node(s7, "User 会话推流\\n保持 Rdpgfx/输入"),
      edge(s1, s2, "->"),
      edge(s2, s3, "->"),
      edge(s3, s4, "->"),
      edge(s4, s5, "->"),
      edge(s5, s6, "->"),
      edge(s6, s7, "->")
    )
  )
)
== 关键接口设计
- CLI/INI：`--config`、`--system`、`--handover`、`--bind-address`、`--port`、`--nla-username/password`、`--pam-service`。INI 提供 `[capture] width/height`, `[encoding] progressive`, `[auth] enable_nla`, `[security] cert/key`。
- System DBus：`org.deepin.RemoteDesktop.Rdp.Dispatcher.RequestHandover`（返回 handover 对象路径）、`Rdp.Handover.StartHandover/TakeClientReady/TakeClient/RedirectClient/GetSystemCredentials/RestartHandover`。
- LightDM DBus（upstream/lightdm-rdp2）：`org.deepin.DisplayManager.RemoteDisplayFactory.CreateRemoteGreeterDisplay/CreateSingleLogonSession`，SeatRDP run_script 时设置 REMOTE_HOST/PAM_RHOST，remote-server 将 session id、remote-id、routing token 回传 drd。
- 会话接口：freerdp_peer PostConnect、Activate、Authenticate、GraphicsPipeline 等回调；input 回调 hooking 到 drd_input_dispatcher。

== 关键数据结构设计
- DrdServerRuntime：capture/encoding/input 单例及 DrdEncodingOptions、transport_mode、ready 标识；对外暴露 prepare_stream、pull_encoded_frame、set_transport。
- DrdRdpSession：freerdp_peer、DrdServerRuntime、DrdRdpGraphicsPipeline、render_thread、frame_sequence、shutdown_cancellable 等。
- DrdRdpGraphicsPipeline：RFX context、capacity_cond/capacity_mutex、outstanding_frames、max_outstanding_frames、needs_keyframe。
- DrdRemoteClient（system 守护）：remote_id、routing_token、GSocketConnection、assigned handover、use_system_credentials。
- DrdTlsCredentials：缓存 PEM 证书/私钥文本，支持 system/handover reload。
- LightDM SeatRDP（upstream）：SeatRDPClass 复写 setup/create_display_server/create_session/run_script，配置 remote 环境变量。

== 数据库设计
=== 数据库设计概述
系统不引入数据库，配置来自 INI + CLI，临时状态驻留内存，LightDM 通过 DBus 交流；若未来需要持久化审计，将单独设计 KV/SQL。

=== 数据库信息
当前无数据库实例；部署中无需维护数据库连接池或权限。

=== 数据库ER图设计
不适用。

=== 数据库表结构
不适用。

=== 表结构约束
不适用。

=== 数据库视图
不适用。

=== 存储过程
不适用。

=== 触发器
不适用。

=== 函数
不适用。

=== 数据安全
未存储数据库数据；凭据通过 TLS/NLA/PAM 即时处理，SAM 文件使用后删除。

== 主要人机交互设计
- CLI：面向管理员，提供模式切换、端口、证书、NLA 参数、日志级别等；`--handover` 模式用于 LightDM 会话。
- INI：`config/default-*.ini` 内含 capture/encoding/auth/security 分段，便于以配置管理工具下发。
- 日志：drd_log_init 安装 g_log writer，以 `domain-level[file:line func]: message` 输出，配合 journald/systemd。
- LightDM：remote-server 通过 DBus 与 drd 通信，SeatRDP 继承 Seat 以 remote 方式运行，界面展示沿用 LightDM greeter。

= 非功能性设计
== 安全性
- 默认启用 TLS + NLA，禁止 RDP Security；SAM 文件按连接生成一次性凭据后 SecureZeroMemory 清理。
- system 模式暴露 GetSystemCredentials，仅当客户端无 RDSTLS 且需系统凭据时由管理员许可。
- PAM 模式（enable_nla=false）同样强制 TLS 通道，并由 security/drd_local_session 负责 open/close session。
- LightDM SeatRDP 通过 REMOTE_HOST/PAM_RHOST 让 logind 标记远程，RemoteDisplayFactory remote-server 通信仅走系统 bus。

== 性能
- capture 队列仅存最新帧，renderer 同步编码，最大限度降低延迟。
- Progressive 采用 RLGR1，SurfaceBits 回退按行分片，确保低带宽可用。
- Rdpgfx 背压使用 capacity_cond + outstanding_frames，避免客户端 ACK 堵塞造成内存膨胀。
- LightDM remote-server 仅在新会话时触发一次 fork/exec，不影响 steady-state。

== 可靠性
- Renderer 异常自动降级 Raw，并请求关键帧恢复；输入注入失败记录日志并继续。
- System/Handover 使用 routing token + DBus queue，多段 handover 都可在 token 匹配后重新领取连接。
- TLS 证书/私钥缓存副本，避免重复释放导致崩溃；LightDM remote-server 失败会触发 SeatRDP 清理，drd 记录告警。

== 易用性
- Meson 一键构建/测试/安装；`meson setup build && meson compile -C build`。
- 配置文件遵循分段结构，适配 config management。
- CLI 带有 `--help` 描述，日志统一格式方便筛查。
- LightDM SeatRDP 直接复用现有 greeter，人机交互一致。

== 可用性
- systemd unit（system/handover/user）提供自动重启与日志整合。
- DBus 接口支持 RestartHandover、RedirectClient，可由桌面环境主动恢复。
- LightDM remote-server 失败时 SeatRDP 会回退到默认行为，保障可回滚。

== 兼容性
- FreeRDP 3.x/mstsc/Remmina 均可连接；SurfaceBits Raw 回退兼容旧客户端。
- 输入层支持扩展扫描码、AltGr、滚轮缩放；LightDM SeatRDP 兼容现有 logind/polkit。
- 配置文件遵循标准 INI，可被现有管理工具处理。

== 韧性
- Rdpgfx 超时/ACK 丢失自动降级 Raw，确保画面可恢复。
- System/Handover crash 后可通过 systemd 重启 + routing token 缓存恢复连接。
- LightDM remote-server 断开时，drd system 守护会重新申请 handover 对象并通知 greeter。

== 隐私
- NLA 凭据、SAM 文件与一次性密码使用完立即擦除；日志严禁输出明文密码。
- TLS 证书路径配置可指向受管密钥仓；remote-server 仅传递 remote-id/states，不含用户密码。
- LightDM SeatRDP 通过 PAM_RHOST 告警潜在来源，便于审计。

== 可维护性
- 目录分层明确（core/capture/encoding/input/security/session/system/transport/utils/doc/upstream），architecture 与本文档同步更新。
- `.codex/plan` 管理任务，doc/changelog.md 记录所有文档/代码变更。
- 建议在 CI 中运行 `meson test -C build --suite unit` 以及 smoke `./build/src/deepin-remote-desktop --config config/default-user.ini`。
- LightDM 扩展保持独立仓库（upstream/lightdm-rdp2），有独立 doc/changelog/Makefile，可单独升级。

= 部署与实施
1. 依赖：`meson`、`ninja`、`pkg-config`、`libglib2.0-dev`、`libsystemd-dev`、`libpam0g-dev`、`libx11-dev`、`libxdamage-dev`、`libxtst-dev`、`freerdp3-dev`、`libwinpr3-dev`。
2. 构建：`meson setup build --prefix=/usr --buildtype=debugoptimized`，随后 `meson compile -C build`。
3. 测试：`meson test -C build --suite unit`，可附加 `--repeat=2 --setup=ci` 暴露竞态。
4. 安装：`meson install -C build --destdir=$PWD/pkgdir`，复制 `pkgdir/usr` 至系统，包含 `/usr/bin/deepin-remote-desktop`、systemd unit、DBus policy、config 模板、certs。
5. LightDM：在目标系统安装 upstream/lightdm-rdp2（`meson setup && ninja -C build && ninja -C build install` 或发行版包），在 `data/lightdm.conf` 启用 `type=rdp` seat，并配置 `remote-server=/usr/lib/deepin-remote-desktop-system`。
6. system 模式：启用 `systemctl enable --now deepin-remote-desktop-system.service`；handover 在 greeter/user session 以 user service 运行。
7. Smoke：`./build/src/deepin-remote-desktop --config ./config/default-user.ini`（用户模式）或 `--system`（system 模式，需配合 LightDM remote-server）。

= 专利
- 当前未计划申请专利，若未来涉及编码/安全算法专利将单独立项评估。

= 附录
- doc/architecture.md：分层、mermaid/sequence 详解。
- doc/02-远程登录实现流程与机制.md：System/Handover 深入说明。
- doc/TODO.md：能力蓝图与任务跟踪。
- upstream/lightdm-rdp2/doc/architecture.md：LightDM SeatRDP/RemoteDisplayFactory 设计。
