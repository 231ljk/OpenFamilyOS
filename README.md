# FamilyOS · 开轮版（Open Wheel Edition）

> An open-source, community-driven operating system base.
> 一个面向全场景设备的开源操作系统底子。代码公开、注释清晰、功能克制、玩法自由。

**仓库**：<https://github.com/231ljk/OpenFamilyOS>
**状态**：Active development, community driven
**许可**：内核相关 GPL-2.0，用户态与工具 MIT（见 [LICENSE](LICENSE)）

---

## 这是什么

FamilyOS 是一个面向**电脑、平板、手机、电视、手表**全场景设备的操作系统项目。
本仓库发布的是**开轮版（Open Wheel Edition）**：

- 基于 **Linux 内核二次开发**，是你自己的系统，不是某个发行版的套壳；
- 「开轮」= 开放、开源、让系统的轮子转起来；
- 功能少、玩法少、代码清晰有注释，方便阅读和改造；
- 不预设壁纸、不预设品牌，给你一个干净的底子；
- 免费，不管控 —— 你拿去干什么我们都不过问。

我们的理念：**让每一个人使用到最好用的系统。科技不是高高在上的，而是服务每一个人。**

## 架构总览

整体思路是「**通用标准 + 自研模块组合**」：

```
┌─────────────────────────────────────────────────┐
│  应用生态  .fos 原生包 / 转包引擎 (APK·deb·rpm) │
├─────────────────────────────────────────────────┤
│  子系统    教育系统（与日常系统并行、数据隔离） │
├─────────────────────────────────────────────────┤
│  核心服务  超级环 · AI模型总线 · 智能家居桥接   │
│            语音助手 · 虚拟化兼容层 · 换机 · 健康│
├─────────────────────────────────────────────────┤
│  存储层    fosfs 自研格式（加密/压缩/快照）     │
│            对外兼容 ext4 / exFAT / NTFS          │
├─────────────────────────────────────────────────┤
│  内核层    Linux 基线 + FamilyOS 可替换模块     │
└─────────────────────────────────────────────────┘
```

| 设计原则 | 落地方式 |
| --- | --- |
| 通用部分兼容行业标准 | IPC、文件系统互通、包格式转换全部对接标准 |
| 自研部分全部自研 | 内核模块、系统服务、缓存、`.fos` 包格式均为本仓库原创代码 |
| AI 不绑定云端 | 统一模型总线接口，默认本地开源模型，可自由替换，不联网、不烧 Token、不上传用户数据 |
| 闭源分支共享设计 | 本仓库的模块接口与设计即闭源自研内核分支的共享框架 |

## 核心模块

| 模块 | 位置 | 一句话说明 |
| --- | --- | --- |
| 超级环 Super Ring | [`services/super-ring/`](services/super-ring) + [`kernel/super_ring/`](kernel/super_ring) | 全局设备协同入口：投屏、互传、剪贴板同步、网络共享，非本生态蓝牙外设也能接入 |
| AI 模型总线 | [`services/ai-bus/`](services/ai-bus) | 统一模型接口层，dlopen 插件式加载，默认搭载可替换的本地模型 |
| 智能家居桥接 | [`services/home-bridge/`](services/home-bridge) | 只做桥接不做硬件；兼容星闪、运营商协议、各厂商私有协议 |
| 语音助手 | [`services/voice/`](services/voice) | 跨端语音操控：设备内控制、跨设备控制与场景联动，模型可替换 |
| 虚拟化兼容层 | [`services/virt-compat/`](services/virt-compat) | ARM64 原生运行 + x86_64 指令翻译；电脑窗口化并行，手机/平板全屏切换 |
| 换机方案 | [`services/migration/`](services/migration) | 双向迁移：迁入 FamilyOS / 迁出 FamilyOS；应用、文件、设置、账号关联 |
| 健康使用设备 | [`services/health/`](services/health) + [`kernel/health_usage/`](kernel/health_usage) | 家长实时查看应用使用情况、随时禁用指定应用，覆盖全设备 |
| fosfs 自研存储 | [`fs/fosfs/`](fs/fosfs) | 系统内部存储格式：加密、压缩、快照、元数据扩展；对外仍兼容 ext4/exFAT/NTFS |
| .fos 包与转包引擎 | [`pkg/`](pkg) | 原生包格式 + APK/deb/rpm 转包，运行在沙箱中 |
| 教育系统 | [`edu/`](edu) | 独立子系统：学校/校外/特色/厂商课程，编程进阶路径，3D 场景闯关 |
| 刷机与维护 | [`tools/`](tools) | 统一工具：镜像写入、备份恢复、环境切换、救砖 |
| 设备支持矩阵 | [`devices/`](devices) | 电脑、平板、手机、电视、手表各设备的适配说明 |

## 快速开始

```bash
# 1. 克隆仓库
git clone https://github.com/231ljk/OpenFamilyOS.git
cd OpenFamilyOS

# 2. 构建用户态服务与内核模块（需要 Linux + gcc + 内核头文件）
make all

# 3. 跑测试（Python 部分在任意平台可跑）
make test

# 4. 刷机与维护工具
python3 tools/familyos_flash/cli.py --help
```

详细的构建说明、依赖列表与目标设备支持矩阵见 [BUILD.md](BUILD.md) 与各设备子目录下的 README。

## 目录导航

```
OpenFamilyOS/
├── kernel/       # 内核二次开发：模块、补丁、配置（GPL-2.0）
├── services/     # 核心系统服务（C, POSIX，MIT）
├── fs/fosfs/     # 自研存储格式规范与参考实现
├── pkg/          # .fos 包格式与转包引擎（Python）
├── edu/          # 教育系统课程与编程路径
├── tools/        # 统一刷机与维护工具（Python）
├── devices/      # 设备支持矩阵与适配说明
├── docs/         # 架构与生态文档
└── .github/      # CI 流水线
```

## 如何参与贡献

开轮版欢迎任何形式的参与：

- **提交 Issue**：报告 Bug、提出功能建议
- **提交 Pull Request**：修复问题、改进代码、补充文档
- **参与文档**：完善使用说明、翻译、教程
- **分享经验**：在论坛、博客、视频中分享你的玩法

提交代码前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)，其中说明了代码风格、提交规范与 CLA（贡献者许可协议）签署方式。

## 版本与路线图

- **当前**：社区版本自用与验证阶段
- **近期**：内部完善、修复问题、补充功能
- **中期**：扩大设备支持范围与生态建设
- **未来**：推进商业化与更广泛的生态合作

开轮版将**始终**作为免费、开放的社区版本存在，不会因商业化而改变其开源属性。版本历史见 [CHANGELOG.md](CHANGELOG.md)。

---

*English summary: FamilyOS Open Wheel Edition is an open-source, Linux-based OS project that is yours to own, fork and ship. It pairs standard compatibility (ext4/exFAT/NTFS, industry IPC norms, package conversion) with self-developed modules: a cross-device coordination hub (Super Ring), a local-first replaceable AI model bus, a smart-home bridge, a voice assistant, a virtualization compatibility layer, bidirectional device migration, per-app health usage controls, the `fosfs` internal storage format, the `.fos` package ecosystem, an isolated education subsystem, and a unified flashing/maintenance toolchain. Free forever, no management, no strings attached.*

—— FamilyOS Lab · [231ljk](https://github.com/231ljk)
