# input 模块 - 输入层

[根目录](../../CLAUDE.md) > [src](../) > **input**

## 模块职责

input 模块负责：
- 键盘事件注入
- 鼠标指针移动与点击
- 滚轮事件处理
- Unicode 字符注入
- RDP 扫描码到 X11 Keycode 映射

## 入口与启动

### 输入分发器

- **头文件**: `drd_input_dispatcher.h`
- **创建**: `drd_input_dispatcher_new()`
- **启动**: `drd_input_dispatcher_start(width, height, error)`
- **停止**: `drd_input_dispatcher_stop()`

### 分发器方法

```c
void drd_input_dispatcher_update_desktop_size(dispatcher, width, height);
gboolean drd_input_dispatcher_handle_keyboard(dispatcher, flags, scancode, error);
gboolean drd_input_dispatcher_handle_unicode(dispatcher, flags, codepoint, error);
gboolean drd_input_dispatcher_handle_pointer(dispatcher, flags, x, y, error);
void drd_input_dispatcher_flush(dispatcher);
```

## 对外接口

### DrdX11Input - X11 注入器

```c
DrdX11Input *drd_x11_input_new(void);
gboolean drd_x11_input_start(input, error);
void drd_x11_input_stop(input);
void drd_x11_input_update_desktop_size(input, width, height);
gboolean drd_x11_input_inject_keyboard(input, flags, scancode, error);
gboolean drd_x11_input_inject_unicode(input, flags, codepoint, error);
gboolean drd_x11_input_inject_pointer(input, flags, x, y, error);
```

### RDP 事件标志

```c
// 键盘标志
KBD_FLAGS_RELEASE = 0x8000
KBD_FLAGS_EXTENDED = 0x0100

// 鼠标标志
PTR_FLAGS_MOVE = 0x8000
PTR_FLAGS_BUTTON1 = 0x1000  // 左键
PTR_FLAGS_BUTTON2 = 0x2000  // 右键
PTR_FLAGS_BUTTON3 = 0x4000  // 中键
PTR_FLAGS_WHEEL = 0x0200
```

## 关键依赖与配置

### 外部依赖

- **X11**: `x11`, `xtst`
- **GLib**: `glib-2.0`, `gobject-2.0`

### 座标缩放

当编码分辨率与真实桌面分辨率不同时，输入座标会被自动缩放：
- RDP 客户端发送的 (x, y) 基于编码分辨率
- 注入器按比例映射到真实桌面分辨率
- `update_desktop_size()` 用于更新真实分辨率

## 数据模型

### DrdInputDispatcher 结构

```
DrdInputDispatcher
├── x11_input: DrdX11Input*
├── encode_width: uint  // 编码分辨率
├── encode_height: uint
├── real_width: uint    // 真实桌面分辨率
├── real_height: uint
└── started: boolean
```

### DrdX11Input 结构

```
DrdX11Input
├── display: Display*
├── window: Window      // 根窗口
├── scale_x: float      // X 轴缩放比例
├── scale_y: float      // Y 轴缩放比例
├── keycode_cache: GHashTable*  // Keysym -> X11 Keycode 缓存
```

## 关键流程

### 键盘事件注入

```
handle_keyboard(flags, scancode):
  1. 提取扩展标志 (flags & 0xE000)
  2. 剥离第 9 位，保留 8-bit scancode
  3. 传入 freerdp_keyboard_get_x11_keycode_from_rdp_scancode()
  4. 如映射失败（返回 0）→ XKeysymToKeycode() 回退
  5. XTestFakeKeyEvent(display, keycode, !is_release, CurrentTime)
  6. XFlush()
```

### Unicode 注入

```
handle_unicode(flags, codepoint):
  1. 转换为 Keysym: keysym = codepoint
  2. 查询 X11 Keycode: keycode = XKeysymToKeycode(display, keysym)
  3. XTestFakeKeyEvent(display, keycode, KEY_PRESS, CurrentTime)
  4. XTestFakeKeyEvent(display, keycode, KEY_RELEASE, CurrentTime)
  5. XFlush()
```

### 鼠标事件注入

```
handle_pointer(flags, x, y):
  1. 缩放座标:
     real_x = x * scale_x
     real_y = y * scale_y
  2. XTestFakeMotionEvent(display, screen, real_x, real_y, CurrentTime)
  3. 提取按钮状态:
     button1 = (flags & PTR_FLAGS_BUTTON1) != 0
     button2 = (flags & PTR_FLAGS_BUTTON2) != 0
     button3 = (flags & PTR_FLAGS_BUTTON3) != 0
  4. 调用 XTestFakeButtonEvent() 模拟点击
  5. XFlush()
```

### 滚轮事件

```
handle_pointer(flags, x, y):
  if (flags & PTR_FLAGS_WHEEL) {
    button4 = (flags & 0x00FF) != 0  // 向上滚动
    button5 = (flags & 0x0100) != 0  // 向下滚动
    XTestFakeButtonEvent(button)
  }
```

### 扩展扫描码处理

部分 RDP 扫描码包含第 9 位 (0xE000)，表示扩展键（如方向键、Ins/Del/Home/End 等）：
- 剥离后传递给 FreeRDP 映射器
- 映射器独立处理 extended 标志
- 如映射失败，回退到 Keysym 查找

## 性能优化

### 座标缩放预计算

启动时计算 `scale_x`/`scale_y`，避免每次事件进行浮点除法。

### Keycode 缓存

维护 `Keysym -> X11 Keycode` 哈希表，避免重复查询 `XKeysymToKeycode()`。

### 批量刷新

`flush()` 一次性调用 `XFlush()`，可合并多个事件减少上下文切换。

## 测试与质量

**当前状态**: 无自动化测试

**建议测试方向**：
1. 键盘事件注入测试（ASCII/扩展键）
2. Unicode 注入测试（各种语言字符）
3. 鼠标移动与点击测试
4. 滚轮事件测试
5. 座标缩放准确性测试
6. 多分辨率切换测试

## 常见问题 (FAQ)

### Q: 为何扩展扫描码需要剥离第 9 位？
A: FreeRDP 映射器仅接受 8-bit scancode，扩展标志通过 flags 参数传递

### Q: 为何需要 Keysym 回退？
A: 部分修饰键（Alt/AltGr 等）在 FreeRDP 旧映射中返回 0，需回退到 X11 直接查找

### Q: Unicode 注入为何先按再放？
A: 模拟实际键盘行为，确保应用正确接收字符

### Q: 座标缩放何时更新？
A: 编码分辨率变化或窗口调整时调用 `update_desktop_size()`

### Q: 如何调试输入问题？
A: 开启 `XTestFakeKeyEvent` 的详细日志或使用 `xev` 工具观测事件

## 相关文件清单

```
src/input/
├── drd_input_dispatcher.h     # 输入分发器
├── drd_input_dispatcher.c
└── drd_x11_input.h            # X11 注入器
```

## 变更记录 (Changelog)

### 2025-12-30 - 模块文档初始化

- 新增 `input/CLAUDE.md` 文档
- 记录键盘/鼠标/Unicode 注入
- 记录扫马码映射与回退机制
- 记录座标缩放与缓存优化
- 标记待补充测试