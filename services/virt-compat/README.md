# virt-compat — 虚拟化兼容层

`fos-virt-compat-d`：让不同架构的应用跨设备运行。

- **ARM64 原生**：同架构直接 exec（native 路径）；
- **x86_64 指令翻译**：异架构应用交给可插拔翻译后端
  （参考实现内置 box64 / fex / qemu-x86_64 三个命令模板，
  `FOS_TRANSLATOR` 环境变量选择）；
- **形态策略**：电脑端 `window` 窗口化并行；手机/平板 `fullscreen`
  全屏切换，`mode.set` 热切。

本服务是「调度与决策」，不重复造翻译轮子。真实系统上请装
Box64/FEX 发行包并把命令模板对准它们。

## IPC 命令

| 命令 | 说明 |
| --- | --- |
| `info` | 架构、窗口模式、可用后端 |
| `app.run {arch,path,args}` | 按架构决策路径并拉起 |
| `mode.set {mode}` | `window` / `fullscreen` |
| `apps.list` | 托管中进程列表 |
