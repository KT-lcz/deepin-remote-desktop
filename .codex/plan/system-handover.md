# 任务：System/Handover 架构对齐

## 上下文
- 需求来源：参考 `upstream/gnome-remote-desktop-48.0` 的 system/handover 设计，Deepin 版本需导出 `org.deepin.*` DBus 接口，并实现 routing token 逻辑。
- 现状：核心监听层已迁移至 `GSocketService`，但只有用户态/system 模式开关，缺少 handover 进程、DBus skeleton 与 routing token 支撑。

## 计划
1. **对齐 DBus 定义与构建产物**：复制 upstream XML，替换前缀为 `org.deepin`，在 Meson 中使用 `gnome.gdbus_codegen` 生成绑定。
2. **扩展运行模式配置**：将 `system_mode` 重构为 `DrdRuntimeMode` 三态，更新配置解析与 CLI。
3. **实现 system/handover 守护结构**：新增守护模块，system 导出 Dispatcher/Handover DBus，handover 仅消费；与现有监听/runtime 串联。
4. **实现 routing token 支撑逻辑**：移植 routing token 解析、handover 协议流程，与新守护协同。
5. **更新文档与验证**：补充架构/变更文档、记录计划完成度，执行 Meson 编译验证。

## 进度
- [x] 步骤 1
- [x] 步骤 2
- [x] 步骤 3（system 端监听委托 + routing token 解析、handover 对象注册、handover 端 Request→TakeClient 流程骨架）
- [ ] 步骤 4（Server Redirection + handover 双重接力细节、PDU/FD 接力、重连 token 管理）
- [ ] 步骤 5
