# migration — 换机方案（双向迁移）

`fos-migration-d`：

- **迁入**：从 Android / iOS / 其他 Linux 设备迁入 FamilyOS；
- **迁出**：从 FamilyOS 迁出到其他系统或标准格式；
- **四类通道**：应用列表（apps）、文件（files）、设置（settings）、
  账号关联（accounts）。

## 迁移包结构（目录形态，便于断点续传与检视）

```
<out>/
├── apps.json        # 应用清单：.fos 包名 + 外部来源(apk/deb)标记
├── files/           # 用户文件树
├── settings.json    # 设置快照（键值，含受限档配置）
└── accounts.json    # 账号关联：仅标识 + token 句柄
                     # ★ 红线：绝不迁移明文密码，句柄由账号服务换取
```

## IPC 命令

| 命令 | 说明 |
| --- | --- |
| `export {out,include?}` | 迁出/备份；include 默认全部四类通道 |
| `import {src}` | 迁入；逐应用装 .fos、文件入 fosfs、设置 merge |
| `status` | 进度轮询 `{state,done,total}` |

## 参考实现的边界

四类通道当前写占位文件即完成（状态机与接口已就绪）。
真实导出器接入点：`cmd_export` 循环体内替换 `write_small`；
真实导入器接入点：`cmd_import` 中按通道分派到 pkg 安装器、
fosfs 写入器、设置服务与账号服务。
