### use_system_credentials

- use_system_credentials 由 system 进程在解析带 routing token 的重连时决定：
- 先根据首次连接采集到的客户端信息判断是否为 MSTSC（grd_session_rdp_is_client_mstsc() 检查 FreeRDP 报告的 OS 类型，src/grd-session-rdp.c (lines 203-212)），
- 再解析 routing token 中的 rdpNegReq 是否启用了 RDSTLS（requested_rdstls，src/grd-rdp-routing-token.c (lines 254-272)）。
- 若客户端是 MSTSC 且未请求 RDSTLS，system 认为它无法安全接受服务器注入的新一次性凭据，
- 于是把 remote_client->use_system_credentials 置为 TRUE 并在发 TakeClientReady 时携带该标记（src/grd-daemon-system.c (lines 605-654)）。
- handover 侧收到 TakeClientReady(true) 后会先调用 GetSystemCredentials，从 system 的 GrdSettings（通常绑定 TPM/GKeyFile 后端）拷贝出长期凭据，并覆盖本地原本的随机一次性凭据，再去调用 TakeClient（src/grd-daemon-handover.c (lines 181-213)）。system 对应的 GetSystemCredentials 处理器只允许这类 handover 调用一次，并返回当前 system 凭据（src/grd-daemon-system.c (lines 400-439)）。

#### 流程差异体现在：

- 凭据来源不同：常规（use_system_credentials=false）时，handover 一直使用 GrdCredentialsOneTime 生成的随机账号（src/grd-credentials-one-time.c (lines 184-203)），并在每次新连接时重建（src/grd-daemon-handover.c (lines 387-412)）；而 true 场景直接改用 system 长期凭据，以匹配 MSTSC 已缓存/展示给用户的账号，避免重定向后因凭据不一致导致 NLA 失败。
安全提示不同：当 handover 被迫使用 system 凭据且当前属于远程登录场景（grd_is_remote_login()），它会弹出“继续使用不安全连接？”对话框或提示，提醒管理员此时没有 RDSTLS，真实系统账号正在透传（src/grd-daemon-handover.c (lines 387-414)）。
- StartHandover 与 Server Redirection 输出：在 GetSystemCredentials 覆盖后，后续 StartHandover/RedirectClient 所嵌入的用户名、密码就是 system 账户，确保 MSTSC 在未启用 RDSTLS 时仍能自动重连（src/grd-daemon-handover.c (lines 248-268), src/grd-daemon-system.c (lines 330-373)）。由于凭据与客户端初始输入一致，MSTSC 不会因为“服务器提供的新凭据”而报错。
- 综上，只有 “MSTSC + 无 RDSTLS” 会触发 use_system_credentials=true，流程上的本质差异是跳过一次性凭据生成/分发，改为向 handover 泄露 system 口令并额外提醒管理员，确保兼容老客户端但也暴露了更多凭据风险。


### routing token和remoteid

#### 关系概览

- remote_id 本质是把 routing_token 加上 D-Bus 前缀 REMOTE_DESKTOP_CLIENT_OBJECT_PATH
  形成的对象路径，例如 /org/gnome/RemoteDesktop/Clients/<token>；映射函数
  get_id_from_routing_token() 与 get_routing_token_from_id() 直接互逆，确保两者一一对应
  （src/grd-daemon-system.c:178、src/grd-daemon-system.c:185）。
- routing_token 是发送给客户端的负载均衡 Cookie，写入 Server Redirection PDU 的 Load
  Balance Info 字段（src/grd-session-rdp.c:325），客户端随后会在下一次 TCP 握手中通过
  Cookie: msts=<token> 头带回，该值被 get_routing_token_without_prefix() 截取出来（src/
  grd-rdp-routing-token.c:152、src/grd-rdp-routing-token.c:247）。

#### 交互流程

- 新连接第一次来到 system daemon 时尚未携带 token，on_incoming_new_connection() 会生成随机 routing_token，立即映射出 remote_id 并插入 remote_clients 表，同时用该 remote_id 去GDM 创建 RemoteDisplay（src/grd-daemon-system.c:537、src/grd-daemon-system.c:665、src/grd-daemon-system.c:676）。此时 remote_id 作为 D-Bus 对象路径被暴露给 UI/配置服务。
- 当任意 handover 控制器调用 StartHandover 时，daemon 通过 get_routing_token_from_id()再次取得数字 token，将其嵌入 grd_session_rdp_send_server_redirection() 所构造的 LB_LOAD_BALANCE_INFO，连同一次性用户密码/证书一起下发给客户端（src/grd-daemon-system.c:339、src/grd-session-rdp.c:325、src/grd-session-rdp.c:332）。
- D-Bus API 文档也说明 RedirectClient 信号需要携带 routing_token 和凭据（src/org.gnome.RemoteDesktop.xml:141、src/org.gnome.RemoteDesktop.xml:154）。
- 客户端收到 Server Redirection 后会断开并重新连入，此时 grd_routing_token_peek_async()在 socket 上 MSG_PEEK 读取 TPKT/x224/routingToken/rdpNegReq，若发现 token 就提前发出 incoming-redirected-connection 信号而不是创建新 session（src/grd-rdp-server.c:184、src/grd-rdp-routing-token.c:196）。若没有 token，则按初始连接路径继续创建 GrdSessionRdp（src/grd-rdp-server.c:190）。
- GrdDaemonSystem 订阅 incoming-redirected-connection，用 get_id_from_routing_token()推导出 remote_id 并查表定位原来的 remote_client，然后把新的 GSocketConnection 挂回，同时依据 token 带来的 requested_rdstls 决定是否可以自动使用系统凭据（src/grd-daemon-system.c:605、src/grd-daemon-system.c:644）。成功匹配后，daemon 在对应 handover 对象上发出 TakeClientReady，通知 dst 侧可以调用 TakeClient()，也就是把 remote_id 映射回具体的 socket（src/grd-daemon-system.c:651）。
- 整个过程中 remote_id 只在 GNOME 侧传播（GDM、D-Bus、日志），routing_token 只在 RDP 协议层传播。二者一一对应，因此 remote_id 是用户态/控制面的稳定键，而 routing_token 是线协议里的物理路由标签。

#### 使用场景与作用

- remote_id 让 org.gnome.RemoteDesktop.Rdp.Handover 的调用者可以通过对象路
  径唯一定位要操作的客户端（授权时还会校验 caller 是否在同一个 session，见
  get_handover_object_path_for_call()，src/grd-daemon-system.c:236），而 routing_token
  的值永远不需要暴露给上层 UI。
- routing_token 作为 Load Balancer Cookie 还承载了部分安全逻辑：如果客户端是 mstsc 且未
  请求 RDSTLS，就允许目标端自动注入系统凭据（src/grd-daemon-system.c:645），从而保证一次
  handover 过程中认证体验一致。
- 日志中“with/without routing token”两类记录直接对应 on_incoming_new_connection() 与
  on_incoming_redirected_connection() 的路径，使问题定位时可以看出当前连接是否属于某个
  remote_id（src/grd-daemon-system.c:631、src/grd-daemon-system.c:665）。

### 端到端的“从协商到发帧”的完整流程
- 以下流程基于 FreeRDP/libfreerdp-server 与 MS-RDPRFX（RemoteFX Progressive）规范的常见实现约定；
具体 API 名称可能随 FreeRDP 版本略有不同，但步骤与数据组织不变。

能力协商与上下文

- 启用表面命令与帧标记：
    - settings->SupportSurfaceCommands = TRUE
    - settings->FrameMarkerCommandEnabled = TRUE
- 广告 Progressive 编解码器（Bitmap Codecs Capability Set）：
    - 在 settings->BitmapCodecs 中添加“Progressive(RFXP)”的 codecGuid 与 codecId（通常为 RDP_CODEC_ID_PROGRESSIVE）。
- 颜色/像素格式：
    - 使用 32bpp BGRA/ARGB（与客户端协商一致），禁用调色板路径。
- Progressive 编码器上下文：
    - 创建并复用 PROGRESSIVE_CONTEXT，例如 progressive_context_new(TRUE)。
    - 使用 64x64 tile 尺寸，设置量化表/质量档位（可用 FreeRDP 默认）。

一帧（frame）发送的消息结构

- 封装在一个 TS_SURFACE_BITS（Surface Bits）命令的 payload 中，codecId = PROGRESSIVE：
    - Progressive Sync（仅首次或上下文重置时）
    - Progressive Context（仅首次或参数/量化变化时）
    - Frame Begin
    - Region（本帧 tile 区域与量化选择）
    - Tile 数据块（通常用 Tile Simple；也可 First + Upgrade 多次渐进）
    - Frame End
- 用 TS_FRAME_MARKER 包裹该帧（Begin/End），方便客户端将解码同步到帧边界，并支持帧 ACK。

逐帧编码与发送流程

- 计算本帧脏矩形集合（ROI），按 64x64 对齐成 tile 网格；得到 tile 坐标列表。
- 如为第一帧或上下文/量化改变：
    - 在 Progressive bitstream 中写入 Sync、Context。
- 写入 Frame Begin。
- 写入 Region：
    - 给出区域包含的 tile 集合与该帧所用量化参数（可全区域统一或分块）。
- 逐 tile 编码与写入：
    - 将源像素转成目标像素格式（通常 BGRA），提供 src, stride。
    - 选择单次发送可用 Tile Simple 块；若做真正渐进：
        - 先发 Tile First（低频、初始质量），随后 1~N 个 Tile Upgrade（高频/残差）块。
- 写入 Frame End。
- 发送包：
    - 用 update->SurfaceFrameMarker(context, TRUE, frameId) 发送帧开始标记
    - 发送 SurfaceBits（codecId=PROGRESSIVE，bitmapData 为上述 Progressive bitstream）
    - 用 update->SurfaceFrameMarker(context, FALSE, frameId) 发送帧结束标记
    - 如启用帧 ACK，则等待或限流到指定“在飞帧”数量

关键数据要点

- TS_SURFACE_BITS 外层矩形应覆盖本帧的 ROI 外接矩形；Progressive Region 决定具体哪些 tile 被更新。
- tile 坐标以 64x64 网格为单位；Region 中是 tile 索引/列表，而不是像素矩形。
- 首帧必须使客户端同步：建议包含 Sync + Context；当量化/参数变化时重发 Context。
- 质量/量化：可直接使用 FreeRDP 的默认量化表，或按实时码率自适应调整；多次 Upgrade 可在网络良好时提高最终质量。

- 连接建立时（能力协商）：
    - 设置 settings->SupportSurfaceCommands = TRUE
    - 设置 settings->FrameMarkerCommandEnabled = TRUE
    - 在 settings->BitmapCodecs 加入 Progressive 编解码器
    - PROGRESSIVE_CONTEXT* prg = progressive_context_new(TRUE);
- 发送一帧：
    - 计算脏区 -> tile 列表；frameId++
    - update->SurfaceFrameMarker(context, TRUE, frameId);
    - wStream* s = Stream_New(NULL, initialCapacity);
    - 如首帧或变化：progressive_write_sync(prg, s); progressive_write_context(prg, s, params);
    - progressive_write_frame_begin(prg, s);
    - progressive_write_region(prg, s, tiles, quant);
    - 对每个 tile：
        - progressive_compress_tile(prg, srcBGRA, stride, tileX, tileY, quality, &tileOut);
        - 简单路径：progressive_write_tile_simple(prg, s, &tileOut);
        - 渐进路径：progressive_write_tile_first(...) 后续 progressive_write_tile_upgrade(...)
    - progressive_write_frame_end(prg, s);
    - 准备 SURFACE_BITS_COMMAND cmd：
        - cmd.codecID = RDP_CODEC_ID_PROGRESSIVE
        - cmd.destLeft/Top/Right/Bottom = ROI 外接矩形
        - cmd.bpp = 32; cmd.width/height = 矩形尺寸
        - cmd.bitmapData = Stream_Buffer(s); cmd.bitmapDataLength = Stream_GetPosition(s);
    - update->SurfaceBits(context, &cmd);
    - update->SurfaceFrameMarker(context, FALSE, frameId);
    - 可选：基于帧 ACK 控制发送节奏
    - Stream_Free(s, FALSE);

多次渐进（可选）

- 单帧可拆成多个 SurfaceBits 发送或单个 SurfaceBits 内多个 tile pass：
    - First pass：低频/粗量化，快速出现画面
    - Upgrade pass(es)：增加高频系数/降低量化，逐步清晰
- 客户端在接收到 Frame End 才会将所有 pass 的结果应用为该帧的最终图像。

常见坑与校验

- 像素格式与 stride 不匹配（推荐统一 BGRA 32bpp）。
- ROI 未按 64 对齐导致 tile 索引错位；务必 tile 对齐并以外接矩形包围。
- 忘记首帧发送 Sync/Context 或参数变化时未重发 Context。
- SurfaceBits 的外层矩形与 Region/tile 列表不一致（容易引起客户端拒收或花屏）。



### 可优化
• 以下模块仍存在可优化空间，按子系统给出分析与后续可行方向：

encoding

- src/encoding/drd_rfx_encoder.c:400+：RFX 编码过程中 collect_dirty_rects() 对每个 tile 逐行 memcmp；可考虑引入 SSE/AVX128 加速或对大块 tile 先使用 g_checksum_update 预判，再在 hash失败时逐行比对。
- Progressive 编码目前固定 keyframe 间隔，缺乏根据 ACK/带宽动态调整的逻辑；可在 drd_rdp_graphics_pipeline 中记录 queueDepth 变化并反馈给 encoder。

session

- src/session/drd_rdp_session.c:380-550：render 线程对 drd_server_runtime_pull_encoded_frame 的错误处理仅记录日志而不统计，
- 可增加计数器并在 session 关闭时输出，以便定位 capture/encoding 瓶颈。

### 运行模式分辨率策略调整

#### 修改目的
- 让 system 模式根据客户端分辨率决定采集/编码尺寸，并在创建 LightDM remote display 时同步传递客户端分辨率。
- 让 user/handover 模式使用当前显示器实际分辨率作为采集与编码目标，避免配置与真实桌面不一致。

#### 修改范围
- 采集模块：新增显示分辨率查询接口（X11 DisplayWidth/Height）。
- runtime 初始化与监听器：根据运行模式动态更新 DrdEncodingOptions 的 width/height。
- system daemon：在 session ready 阶段读取客户端分辨率并更新 runtime，同时传递给 LightDM。

#### 修改内容
- 提供 drd_x11_capture_get_display_size 与 drd_capture_manager_get_display_size 供 runtime 读取实际分辨率。
- drd_application_prepare_runtime 在非 system 模式下改用实时显示分辨率写入 runtime 编码选项。
- system 模式下在能力协商/会话就绪时读取客户端分辨率，更新 runtime 编码参数并作为 LightDM remote display 的宽高参数。

#### 对整体项目的影响
- 提升 system 模式下远程登录分辨率适配性，避免固定分辨率导致画面拉伸或缩放。
- user/handover 模式的捕获与编码与真实显示器一致，减少配置错误引起的黑边或裁切问题。
