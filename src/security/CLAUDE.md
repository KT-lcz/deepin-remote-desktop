# security 模块 - 安全层

[根目录](../../CLAUDE.md) > [src](../) > **security**

## 模块职责

security 模块负责：
- TLS 证书与私钥加载
- NLA (Network Level Authentication) SAM 文件生成
- PAM 会话创建与销毁
- 凭据安全传递与擦除

## 入口与启动

### TLS 凭据类

- **头文件**: `drd_tls_credentials.h`
- **创建**:
  - `drd_tls_credentials_new(certificate_path, private_key_path, error)` - 从文件加载
  - `drd_tls_credentials_new_empty()` - 创建空凭据（用于 handover PE M 重载）

- **应用**: `drd_tls_credentials_apply(settings, error)` - 注入 FreeRDP settings

### 获取原始凭证

```c
rdpCertificate *drd_tls_credentials_get_certificate(DrdTlsCredentials *self);
rdpPrivateKey *drd_tls_credentials_get_private_key(DrdTlsCredentials *self);
```

### PEM 重载（handover 场景）

```c
gboolean drd_tls_credentials_reload_from_pem(
    DrdTlsCredentials *self,
    const gchar *certificate_pem,
    const gchar *key_pem,
    GError **error
);
```

system 端生成一次性 PEM 直接喂给 handover，避免使用共享文件。

## 对外接口

### NLA SAM 文件

- **头文件**: `drd_nla_sam.h`
- **功能**: 生成临时 NT 哈希 SAM 文件供 CredSSP 使用
- **关键方法**:
  - `drd_nla_sam_file_new(username, password, error)` - 创建并返回文件路径
  - 文件使用 `g_mkstemp()` 生成，PostConnect 后立即删除

### PAM 本地会话

- **头文件**: `drd_local_session.h`
- **功能**: TLS-only 模式下执行 PAM 认证与会话创建
- **关键方法**:
  - `drd_local_session_authenticate(username, password, service, error)` - 认证
  - `drd_local_session_open_error(session, error)` - 打开会话
  - `drd_local_session_close(session)` - 关闭会话并擦除凭据

## 关键依赖与配置

### 外部依赖

- **GLib**: `glib-2.0`
- **FreeRDP**: `freerdp/freerdp.h`
- **PAM**: `libpam0g`

### 安全参数

配置文件中的安全参数：

```ini
[tls]
certificate=/usr/share/deepin-remote-desktop/certs/server.crt
private_key=/usr/share/deepin-remote-desktop/certs/server.key

[auth]
enable_nla=true          # 启用/禁用 NLA
username=uos             # NLA 用户名
password=1               # NLA 密码
pam_service=deepin-remote-sso  # PAM service
```

## 数据模型

### DrdTlsCredentials 结构

```
DrdTlsCredentials
├── certificate_path: string
├── private_key_path: string
├── certificate: rdpCertificate*
├── private_key: rdpPrivateKey*
├── certificate_pem: string  # PEM 数据缓存（用于 handover）
└── key_pem: string
```

### DrdLocalSession 结构

```
DrdLocalSession
├── username: string
├── service: string
├── pam_handle: pam_handle_t*
└── authenticated: boolean
```

## 关键流程

### TLS+NLA 认证链路

```
1. DrdConfig 读取 username/password
2. DrdTlsCredentials 加载证书/私钥
3. DrdNlaSamFile 生成临时 SAM 文件
4. FreeRDP settings 注入:
   - RdpServerCertificate
   - RdpServerPrivateKey
   - FreeRDP_NtlmSamFile
   - NlaSecurity = TRUE
5. CredSSP 从 SAM 读取 NT 哈希
6. 认证成功 → PostConnect → 删除 SAM 文件
```

### TLS-only + PAM 单点登录

```
1. enable_nla=false
2. 监听器设置 NlaSecurity = FALSE
3. 客户端发送 Client Info（含用户名/密码）
4. DrdLocalSession:
   - pam_start(auth_service, username)
   - pam_authenticate(password)
   - pam_acct_mgmt()
   - pam_open_session()
5. 会话激活 → renderer 运行
6. 断开 → pam_close_session() + 擦除凭据
```

### Handover TLS 继承

```
1. System 端加载证书文件
2. Handover 端调用 StartHandover()
3. System 端调用 read_material() → PEM 字符串
4. Handover 接收 PEM → reload_from_pem()
5. Handover TLS 凭据与 System 完全一致
6. Server Redirection PDU 发送
7. 客户端重连（相同 TLS 身份）→ 握手成功
```

## 安全考虑

### 凭据擦除

- SAM 文件生成后使用 `g_autofree` 自动删除
- PAM 密码使用 `pam_set_item(PAM_AUTHTOK)` 后立即调用 `pam_setcred(PAM_DELETE_CRED)`
- 不在日志中输出敏感信息（使用 `**` 掩码）

### TLS 证书

- 开发证书位于 `data/certs/` 仅供本地测试
- 生产环境应使用自签名或 CA 签名证书
- 建议定期轮换证书

### Handover 安全

- routing token 随机生成，防篡改
- 手over 队列限制 32 个连接，超 30 秒自动清理
- System 端不暴露真实密码，仅传递一次性凭据

## 测试与质量

**当前状态**: 无自动化测试

**建议测试方向**：
1. TLS 证书加载测试（有效/无效）
2. SAM 文件生成与清理测试
3. PAM 认证流程测试（成功/失败）
4. PEM 重载测试（handover 流程）
5. 凭据擦除测试（断开后无残留）

## 常见问题 (FAQ)

### Q: 为何需要 SAM 文件？
A: FreeRDP 的 CredSSP 仅支持从 SAM 文件读取 NT 哈希，临时文件避免持久化

### Q: TLS-only 与 NLA 安全性对比？
A: TLS-only 依赖 PAM 任意 username/password，NLA 仅接受固定账号，风险取决于 PAM 配置

### Q: Handover 时如何保证 TLS 身份一致？
A: System 端传递 PEM 字符串而非文件，Handover 端重建相同凭证对象

### Q: PAM 会话何时销毁？
A: 断开连接或会话销毁时自动调用 `pam_close_session()`

## 相关文件清单

```
src/security/
├── drd_tls_credentials.h   # TLS 凭据接口
├── drd_tls_credentials.c   # TLS 凭据实现
├── drd_nla_sam.h           # NLA SAM 接口
├── drd_nla_sam.c           # NLA SAM 实现
├── drd_local_session.h     # PAM 会话接口
└── drd_local_session.c     # PAM 会话实现
```

## 变更记录 (Changelog)

### 2025-12-30 - 模块文档初始化

- 新增 `security/CLAUDE.md` 文档
- 记录 TLS 凭据管理、NLA SAM、PAM 会话
- 记录 handover TLS 继承流程
- 标记待补充测试