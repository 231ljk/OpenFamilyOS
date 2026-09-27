# 更新日志（CHANGELOG）

本仓库遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与
[语义化版本](https://semver.org/lang/zh-CN/)。

## 路线图

| 阶段 | 内容 |
| --- | --- |
| 当前 | 社区版本自用与验证阶段 |
| 近期 | 内部完善、修复问题、补充功能 |
| 中期 | 扩大设备支持范围与生态建设 |
| 未来 | 推进商业化与更广泛的生态合作 |

开轮版将始终作为免费、开放的社区版本存在，不会因商业化而改变其开源属性。

---

## [Unreleased]

### Added
- 仓库骨架：内核模块、七大核心服务、fosfs 存储格式规范与参考实现
- `.fos` 包格式 v1 规范与转包引擎（APK / deb → .fos）
- 统一刷机与维护工具 `familyos_flash`（镜像写入、备份恢复、环境切换、救砖）
- 教育系统目录结构：四类课程样例、编程进阶路径、3D 闯关模块骨架
- 设备支持矩阵：pc / tablet / phone / tv / watch
- CI：内核模块编译、C 服务构建与冒烟测试、Python 单元测试

## [0.1.0] - 2026-09-27

### Added
- 开轮版首次开源发布（Open Wheel Edition）
- 基于 Linux 原生内核（不魔改基线）的可替换模块框架
- 超级环 / AI 模型总线 / 智能家居桥接 / 语音助手 / 虚拟化兼容层 / 换机 / 健康使用 七个核心服务的参考实现

[Unreleased]: https://github.com/231ljk/OpenFamilyOS/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/231ljk/OpenFamilyOS/releases/tag/v0.1.0
