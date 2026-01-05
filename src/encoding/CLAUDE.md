# encoding 模块 - 编码层

[根目录](../../CLAUDE.md) > [src](../) > **encoding**

## 模块职责

encoding 模块负责：
- 统一编码配置与调度
- RemoteFX Progressive 编码
- 帧差分与脏矩形检测
- 编码模式切换与回退

## 入口与启动

### 编码管理器

- **头文件**: `drd_encoding_manager.h`
- **创建**: `drd_encoding_manager_new()`
- **准备**: `drd_encoding_manager_prepare(encoding_options, error)`
- **重置**: `drd_encoding_manager_reset()` - 强制关键帧

### 编码接口

```c
gboolean drd_encoding_manager_encode(
    DrdEncodingManager *self,
    DrdFrame *input,
    gsize max_payload,
    DrdFrameCodec desired_codec,
    DrdEncodedFrame **out_frame,
    GError **error
);
```

## 对外接口

### Surface GFX 编码

```c
gboolean drd_encoding_manager_encode_surface_gfx(
    DrdEncodingManager *self,
    rdpSettings *settings,
    RdpgfxServerContext *context,
    DrdFrame *input,
    guint32 frame_id,
    DrdEncodedFrame **out_frame,
    GError **error
);
```

用于 Rdpgfx 管线的 RemoteFX/Progressive 编码。

### Surface Bits 编码

```c
gboolean drd_encoding_manager_encode_surface_bit(
    DrdEncodingManager *self,
    rdpSettings *settings,
    DrdFrame *input,
    gsize max_payload,
    DrdEncodedFrame **out_frame,
    GError **error
);
```

用于 Surface Bits 的 RemoteFX 编码发送。

### 辅助方法

```c
DrdFrameCodec drd_encoding_manager_get_codec(DrdEncodingManager *self);
void drd_encoding_manager_force_keyframe(DrdEncodingManager *self);
gboolean drd_encoding_manager_is_ready(DrdEncodingManager *self);
gboolean drd_encoder_prepare(DrdEncodingManager *self, guint32 codecs, rdpSettings *settings);
```

## 关键依赖与配置

### 外部依赖

- **GLib**: `glib-2.0`, `gobject-2.0`
- **FreeRDP**: `freerdp/server/rdpgfx.h`, `freerdp/codec/rfx.h`

### 编码配置

```ini
[encoding]
mode=rfx              # rfx | h264 | auto
enable_diff=true      # 启用帧差分
```

### DrdEncodingOptions

```c
typedef enum {
    DRD_FRAME_CODEC_AUTO = 0,
    DRD_FRAME_CODEC_RAW,  # 已废弃
    DRD_FRAME_CODEC_RFX,
    DRD_FRAME_CODEC_H264,
} DrdFrameCodec;
```

## 数据模型

### DrdEncodingManager 结构

```
DrdEncodingManager
├── rfx/progressive/h264 context
├── gfx/surface diff 缓冲
├── codec: DrdFrameCodec
└── enable_diff: boolean
```

### 编码上下文状态

```
RFX/Progressive/H264 Context
├── surface/gfx tile hash
├── previous_frame
└── force_keyframe 标记
```

## 关键流程

### 编码管理器调度

```
encode(input, desired_codec, max_payload):
  1. 锁管理器
  2. 根据 desired_codec 选择编码器
  3. 调用编码器具体实现
  4. 解锁并返回 DrdEncodedFrame
```

### RemoteFX 差分编码

```
drd_encoding_manager_encode_surface_gfx():
  1. 初始化 diff 状态 (tile hash + previous frame)
  2. 如果强制关键帧或禁用差分 → 全帧矩形
  3. 否则调用 collect_dirty_rects():
     - 遍历 64x64 tiles
     - 哈希比较 + 逐行校验
     - 输出 RFX_RECT[] 列表
  4. 如果无变化 → 跳过编码
  5. rfx_compose_message(RLGR1 Progressive)
  6. 更新 previous frame + tile hash
```

### Progressive 帧封装

```
write_progressive_message():
  1. 写入 SYNC
  2. 写入 CONTEXT
  3. 写入 FRAME_BEGIN
  4. 写入 REGION (包含 RFX_RECT[])
  5. 写入 TILE (每个 tile 的压缩数据)
  6. 写入 FRAME_END
```

### SurfaceBits RemoteFX

```
encode_surface_bit():
  1. RemoteFX 编码
  2. 检查 payload 限制
  3. 发送 SurfaceFrameMarker/SurfaceBits
```

### 编码模式切换

当 Rdpgfx 不可用时自动降级到 SurfaceBits RemoteFX。

### 多片段限制

SurfaceBits 超过 payload 限制直接返回错误并触发上层策略。

## 性能优化

### 脏矩形检测

- 使用 tile 哈希快速过滤未变化 tile
- 仅对可疑 tile 逐行校验
- 直接生成 RFX_RECT 输出，减少中间列表

### RLGR1 vs RLGR3

- Progressive 路径默认 RLGR1（与 mstsc/gnome-remote-desktop 一致）
- Surface Bits 回退使用 RLGR3（更高压缩率）

### 缓存与复用

- 脏矩形缓存减少分配抖动
- tile hash 缓存避免重复计算

## 测试与质量

**当前状态**: 无自动化测试

**建议测试方向**：
1. 编码器准备与重置测试
2. 差分编码准确性测试（比对原始/差分）
3. Progressive 封装完整性测试
4. 多片段回退测试
5. 关键帧强制测试

## 常见问题 (FAQ)

### Q: 为何 Progressive 使用 RLGR1 而非 RLGR3？
A: 确保与 Windows mstsc 和 gnome-remote-desktop 兼容性

### Q: 如何强制关键帧？
A: 调用 `drd_encoding_manager_force_keyframe()` 或 `drd_server_runtime_request_keyframe()`

### Q: 差分编码何时禁用？
A: 配置 `[encoding] enable_diff=false` 或编码模式切换时

### Q: 编码器如何知道是否就绪？
A: `drd_encoding_manager_is_ready()` 检查 codec 已设置

### Q: RFX 编码失败时如何回退？
A: 自动切换到 SurfaceBits RemoteFX 路径发送

## 相关文件清单

```
src/encoding/
├── drd_encoding_manager.h    # 编码管理器接口
├── drd_encoding_manager.c    # 编码管理器实现
└── drd_encoding_manager.h    # 编码管理器接口
```

### 文档

```
doc/
├── collect_dirty_rects.md    # 脏矩形检测详解
└── architecture.md           # 架构总览（含编码章节）
```

## 变更记录 (Changelog)

### 2025-12-30 - 模块文档初始化

- 新增 `encoding/CLAUDE.md` 文档
- 记录编码调度与 RemoteFX/Progressive 编码路径
- 记录差分检测与脏矩形算法
- 记录 Progressive 封装格式
- 标记待补充测试
