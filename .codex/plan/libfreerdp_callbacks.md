# 任务：libfreerdp_callbacks

## 上下文
- 需求：从 libfreerdp 角度（脱离具体项目）说明 `freerdp_listener` 与 `freerdp_peer` 的回调函数职责、触发条件与典型实现要点。
- 参考资料：`/usr/include/freerdp3/freerdp/listener.h`、`/usr/include/freerdp3/freerdp/peer.h` 及通用 FreeRDP server 流程。

## 计划
- [x] 查阅 libfreerdp 头文件，确认 listener/peer 可用回调列表
- [x] 归纳 `freerdp_listener` 回调的生命周期、触发条件、常见实现职责
- [x] 归纳 `freerdp_peer` 回调的生命周期、触发条件、常见实现职责并准备回答

## 进度标记
- 已完成：listener/peer 回调整理，并形成回答
- 遗留风险：不同 FreeRDP 版本回调存在差异，回答需要注明基于 3.x API
