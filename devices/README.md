# devices — 目标设备支持矩阵

开轮版面向**全场景设备**。每种设备一个子目录，内含该设备的
README（配方/已知问题）与 `flash.yaml`（刷机参数声明，
被 `tools/familyos_flash devices` 读取）。

## 支持矩阵

| 设备 | 代号目录 | 内核基线 | 形态策略（virt-compat） | 健康使用 | 状态 |
| --- | --- | --- | --- | --- | --- |
| 电脑 | [`pc/`](pc) | Linux 6.6 LTS x86_64 | `window` 窗口化并行 | 前台时长 | 参考支持 |
| 平板 | [`tablet/`](tablet) | Linux 6.6 LTS arm64 | `fullscreen` 全屏切换 | 前台时长+护眼 | 参考支持 |
| 手机 | [`phone/`](phone) | Linux 6.6 LTS arm64 | `fullscreen` | 全通道 | 实验中 |
| 电视 | [`tv/`](tv) | Linux 6.6 LTS arm64/x86_64 | 大屏+遥控器（超级环 ctrl） | 儿童档 | 参考支持 |
| 手表 | [`watch/`](watch) | 裁剪基线 arm64 | 表盘+提醒端 | 仅聚合上报 | 概念验证 |

「参考支持」= 仓库内配置与文档齐备，可按 [BUILD.md](../BUILD.md) 流程构建；
「实验中」= 接口就绪，设备特定驱动适配中；「概念验证」= 架构评估阶段。

## 每设备目录结构

```
devices/<name>/
├── README.md      构建配方、依赖、刷入步骤、已知问题
├── flash.yaml     刷机声明（slots/分区表/救砖入口）
└── rootfs/        （可选）该设备 rootfs 模板与裁剪清单
```

## flash.yaml 字段（被 familyos_flash devices 读取）

| 字段 | 说明 |
| --- | --- |
| `device` | 设备 id（与镜像头 `device` 对齐） |
| `slots` | `_a/_b`（A/B）或 `single` |
| `partition_table` | 分区表声明文件名（偏移+大小） |
| `rescue_entry` | 救砖模式进入方式（按键组合/USB 引导） |
| `notes` | 一行备注 |

## 教育子系统设备侧重

教育系统（`edu/`）主要面向**平板、电脑、电视**；手机/手表作为
家长提醒端与遥控器，见各设备 README 的「教育」小节。
