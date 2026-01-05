# utils 模块 - 工具层

[根目录](../../CLAUDE.md) > [src](../) > **utils**

## 模块职责

utils 模块提供项目通用的基础设施：
- 帧结构 (DrdFrame)
- 编码帧结构 (DrdEncodedFrame)
- 线程安全的帧队列 (DrdFrameQueue)
- 统一日志系统 (DRD_LOG_*)
- 捕获/渲染帧率观测配置

## 入口与启动

### 日志初始化

- **头文件**: `drd_log.h`
- **初始化**: `drd_log_init()` - 在 `main()` 中调用

### 帧率观测配置

- **头文件**: `drd_capture_metrics.h`
- **配置**: `drd_capture_metrics_apply_config(target_fps, stats_interval_sec)`

## 对外接口

### DrdFrame - 原始帧

```c
DrdFrame *drd_frame_new(void);
void drd_frame_configure(frame, width, height, stride, timestamp);
guint8 *drd_frame_ensure_capacity(frame, size);
const guint8 *drd_frame_get_data(frame, size);
```

### DrdEncodedFrame - 编码帧

```c
DrdEncodedFrame *drd_encoded_frame_new(void);
void drd_encoded_frame_configure(frame, width, height, stride,
                                  is_bottom_up, timestamp, codec);
void drd_encoded_frame_set_quality(frame, quality, qp, is_keyframe);
gboolean drd_encoded_frame_set_payload(frame, data, size);
gboolean drd_encoded_frame_fill_payload(frame, size, writer, user_data);
```

### DrdFrameQueue - 帧队列

```c
DrdFrameQueue *drd_frame_queue_new(void);
void drd_frame_queue_push(queue, frame);
gboolean drd_frame_queue_wait(queue, timeout_us, out_frame);
guint64 drd_frame_queue_get_dropped_frames(queue);
```

### DrdCaptureMetrics - 帧率观测

```c
void drd_capture_metrics_apply_config(target_fps, stats_interval_sec);
guint drd_capture_metrics_get_target_fps(void);
gint64 drd_capture_metrics_get_target_interval_us(void);
gint64 drd_capture_metrics_get_stats_interval_us(void);
```

## 关键依赖与配置

### 外部依赖

- **GLib**: `glib-2.0`, `gobject-2.0`

### 日志级别

```c
DRD_LOG_DEBUG(...)     // 调试信息
DRD_LOG_MESSAGE(...)   // 一般信息
DRD_LOG_WARNING(...)   // 警告
DRD_LOG_ERROR(...)     // 错误
```

## 数据模型

### DrdFrame 结构

```
DrdFrame
├── width: uint
├── height: uint
├── stride: uint
├── timestamp: uint64
└── data: uint8*  // BGRX 像素数据
```

### DrdEncodedFrame 结构

```
DrdEncodedFrame
├── width: uint
├── height: uint
├── stride: uint
├── is_bottom_up: boolean
├── timestamp: uint64
├── codec: DrdFrameCodec
├── quality: uint8
├── qp: uint8
├── is_keyframe: boolean
└── payload: uint8*  // 编码后的 RFX/H.264/Raw 数据
```

### DrdFrameQueue 结构

```
DrdFrameQueue (容量 3)
├── buffer: DrdFrame*[3]
├── head: uint
├── tail: uint
├── size: uint
├── dropped_frames: uint64
├── cond: GCond
└── lock: GMutex
```

## 关键流程

### 日志系统

```c
drd_log_init():
  1. g_log_set_writer_func(drd_log_writer)
  2. DRD_LOG_* 宏通过 g_log_structured_standard 注入元信息
  3. drd_log_writer 在栈上拼装格式化字符串
  4. 直接 write(STDERR_FILENO)，避免 GLib 转换重入问题
```

### 帧队列操作

```
push(frame):
  1. 加锁
  2. if (size == capacity) {
       3. 丢弃最旧帧 (buffer[tail]++)
       4. dropped_frames++
     }
  3. 插入新帧
  4. 广播 cond

wait(timeout):
  1. 加锁
  2. while (size == 0)
       3. cond_timed_wait(timeout)
  4. 取出 buffer[tail]
  5. tail++ (环形指针)
  6. size--
```

### Payload 写入

**set_payload()**: 直接复制编码结果
**fill_payload()**: 通过回调自定义写入

Raw 编码器使用 fill_payload 实现行翻转回调：
```c
frame_encoder_fill_payload(
    output,
    size,
    [](dest, size, user_data) -> gboolean {
        // 逐行翻转像素并写入
        return TRUE;
    },
    &context
);
```

## 线程安全

- `DrdFrameQueue` 使用 `GMutex` + `GCond` 保护缓冲区操作
- `drd_frame_queue_wait()` 支持超时防止死锁
- `drd_frame_queue_stop()` 会立即唤醒所有等待者

## 测试与质量

**当前状态**: 无自动化测试

**建议测试方向**：
1. 帧队列推入/弹出测试
2. 丢帧计数准确性测试
3. 超时等待测试
4. Payload 写入回调测试
5. 日志格式一致性测试

## 常见问题 (FAQ)

### Q: 为何队列容量只有 3 帧？
A: 平衡内存占用与延迟，丢帧时 encoder 仍有 1-2 帧缓冲

### Q: 如何减少丢帧？
A: 优化编码器性能、降低分辨率/帧率，或启用差分减少数据量

### Q: Payload 是否会被复制？
A: `set_payload()` 会复制，`fill_payload()` 通过回调直接写入

### Q: 日志为何不使用 g_printerr？
A: 避免 GLib 内部的 locale 转换造成内存分配重入崩溃

### Q: 帧率观测如何工作？
A: Capture/Renderer 定期统计实际帧率，对比目标值并输出日志

## 相关文件清单

```
src/utils/
├── drd_log.h                  # 日志系统
├── drd_log.c
├── drd_frame.h                # 原始帧
├── drd_frame.c
├── drd_encoded_frame.h        # 编码帧
├── drd_encoded_frame.c
├── drd_frame_queue.h          # 帧队列
├── drd_frame_queue.c
└── drd_capture_metrics.h      # 帧率观测配置
```

## 变更记录 (Changelog)

### 2025-12-30 - 模块文档初始化

- 新增 `utils/CLAUDE.md` 文档
- 记录帧结构、队列、日志系统
- 记录线程安全机制
- 标记待补充测试