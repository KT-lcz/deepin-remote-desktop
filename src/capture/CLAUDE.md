# capture 模块 - 采集层

[根目录](../../CLAUDE.md) > [src](../) > **capture**

## 模块职责

capture 模块负责：
- X11 屏幕捕获
- XDamage 事件监听
- 帧队列管理
- 捕获线程控制
- 帧率统计与观测

## 入口与启动

### 捕获管理器

- **头文件**: `drd_capture_manager.h`
- **创建**: `drd_capture_manager_new()`
- **启动**: `drd_capture_manager_start(width, height, error)`
- **停止**: `drd_capture_manager_stop()`
- **等待帧**: `drd_capture_manager_wait_frame(timeout_us, out_frame, error)`

## 对外接口

### 管理器方法

```c
gboolean drd_capture_manager_is_running(DrdCaptureManager *self);
DrdFrameQueue *drd_capture_manager_get_queue(DrdCaptureManager *self);
```

### 捕获选项

配置文件中的捕获参数：

```ini
[capture]
width=1920              # 捕获宽度
height=1080             # 捕获高度
target_fps=60           # 目标帧率
stats_interval_sec=5    # 统计间隔（秒）
```

## 关键依赖与配置

### 外部依赖

- **X11**: `x11`, `xext`, `xdamage`, `xfixes`
- **GLib**: `glib-2.0`

### 环境要求

- 必须访问 X11 DISPLAY
- XDamage 扩展必须可用
- 需要合适的分辨率与刷新率支持

## 数据模型

### DrdCaptureManager 结构

```
DrdCaptureManager
├── capture: DrdX11Capture*       # X11 捕获器
├── queue: DrdFrameQueue*         # 3 帧环形缓冲
├── running: boolean
└── lock: GMutex
```

### DrdX11Capture 结构

```
DrdX11Capture
├── display: Display*
├── window: Window                # 根窗口
├── width: uint16
├── height: uint16
├── target_interval_us: gint64    # 1/fps * 1e6
├── damage_event_base: int
├── damage_error_base: int
├── thread: GThread*              # 捕获线程
├── wakeup_pipe: int[2]           # 唤醒管道
├── running: boolean
├── last_stats_time_us: gint64
├── captured_frames: uint
└── target_fps: uint
```

## 关键流程

### 捕获线程驱动

```
while (running) {
  // 1. 等待目标间隔（基于 g_poll）
  fd_set = { X11 fd, wakeup_pipe[0] }
  poll(fds, target_interval)

  // 2. 消费所有 XDamage 事件（清理/合并）
  while (XPending(display)) {
    XNextEvent(event)
    if (damage) 合并到 region
  }

  // 3. 抓取帧（XShmGetImage）
  frame = XShmGetImage(width, height)

  // 4. 推入帧队列
  frame_queue_push(frame)  // 满 -> 丢弃最旧帧

  // 5. 统计帧率（每 stats_interval_sec）
  if (now - last_stats_time > interval)
    log(fps, target_fps, 达标状态)
}
```

### 停止与唤醒

```
drd_capture_manager_stop():
 1. 标记 running = FALSE
 2. 写入 wakeup_pipe[1]
 3. thread 被 poll 唤醒 → 退出循环
 4. g_thread_join()
```

### 帧队列管理

```
DrdFrameQueue (容量 3)
├── buffer: DrdFrame*[3]
├── head: uint
├── tail: uint
├── size: uint
└── dropped_frames: uint  # 累计丢帧数

push():
  if (size == capacity) {
    dropped_frames++
    evict oldest
  }
  insert new
  size++

wait_frame(timeout):
  cond_wait(timeout)
  return buffer[tail]
```

### XDamage 事件处理

XDamage 事件仅用于清理/合并损坏区域：
- 所有 damage 事件被消费并合并到 REGION16
- 捕获由 `target_interval` 固定周期驱动
- 避免 compositor 低频 damage 限制帧率

## 性能观测

### 帧率统计

每 `stats_interval_sec` 输出：

```
capture fps: 58.3 / 60 target [REACHED]
```

或

```
capture fps: 45.2 / 60 target [BELOW]
```

### 丢帧计数

可通过 `drd_frame_queue_get_dropped_frames()` 获取累计丢帧数：
- 丢帧通常表示编码器背压严重
- 建议监控此指标评估系统负载

## 测试与质量

**当前状态**: 无自动化测试

**建议测试方向**：
1. 捕获线程启动与停止测试
2. 帧队列推入与阻塞测试
3. 目标帧率准确性测试
4. XDamage 合并测试
5. 多分辨率切换测试

## 常见问题 (FAQ)

### Q: 为何使用固定周期驱动而非 XDamage 直接触发？
A: XDamage 频率受 compositor 限制，固定周期确保稳定帧率

### Q: 帧队列为何只有 3 帧？
A: 单帧缓存 + 2 帧冗余，减少内存占用与延迟，丢帧时 encoder 仍有缓冲

### Q: 如何减少丢帧？
A: 优化编码器性能、降低分辨率/帧率，或启用差分减少数据量

### Q: 捕获线程为何需要 wakeup 管道？
A: stop() 时通过写入管道唤醒 XNextEvent()，避免长时间阻塞

### Q: 如何调试捕获性能？
A: 开启 `G_MESSAGES_DEBUG=all` 查看帧率统计和丢帧日志

## 相关文件清单

```
src/capture/
├── drd_capture_manager.h    # 捕获管理器接口
├── drd_capture_manager.c    # 捕获管理器实现
├── drd_x11_capture.h        # X11 捕获器接口
└── drd_x11_capture.c        # X11 捕获器实现
```

## 变更记录 (Changelog)

### 2025-12-30 - 模块文档初始化

- 新增 `capture/CLAUDE.md` 文档
- 记录 X11/XDamage 捕获机制
- 记录固定周期驱动与帧队列
- 记录性能观测指标
- 标记待补充测试