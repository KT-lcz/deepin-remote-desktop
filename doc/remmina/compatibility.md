# Remmina 兼容性补充说明

## 背景
- Remmina/FreeRDP 2024+ 版本会根据服务器宣告的 DisplayControl/MonitorLayout 能力尝试窗口缩放，若服务端未实现完整的显示重配置逻辑，会导致 `Invalid surface bits command rectangle`。
- `rdp-cpp` 在 `c982bcce` 中通过关闭相关能力并在会话激活时强制回写桌面尺寸解决了该问题。本次改动在 `glib-rewrite` 中对齐该行为。

## 同步策略
1. `transport/grdc_rdp_listener` 在 `freerdp_peer` 初始化阶段写入编码宽高、启用 `FreeRDP_DesktopResize`，同时关闭 `SupportMonitorLayoutPdu`/`SupportDisplayControl`。
2. `session/grdc_rdp_session_activate()` 触发新的 `grdc_rdp_session_enforce_peer_desktop_size()`：
   - 再次读取 `GrdcServerRuntime` 的编码宽高，与当前 `rdpSettings` 对比。
   - 若发现客户端尝试更改尺寸，则重写 `FreeRDP_DesktopWidth/Height` 并调用 `DesktopResize`。
3. 一旦 `DesktopResize` 成功，服务端立即记录日志，提示客户端被强制同步至原始分辨率。

## 关键注意事项
- DesktopResize 仅在激活阶段触发一次，若未来提供动态分辨率能力，需要扩展监听器以在 DisplayControl PDU 到来时复用该逻辑。
- 由于 DisplayControl/MonitorLayout 被禁用，Remmina 会将桌面视为静态尺寸；若用户需要手动缩放，应通过客户端缩放功能而非 DisplayControl。
- 日志使用 `g_message` 输出，方便 QA 在问题复现时确认是否执行了强制同步。
