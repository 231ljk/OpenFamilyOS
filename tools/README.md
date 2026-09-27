# tools/ — 统一刷机与维护工具

`familyos_flash`：开发文档第八章「支持系统镜像写入 / 数据备份与恢复 /
环境切换 / 救砖模式」的落地 CLI。纯标准库，Linux/macOS/Windows 均可运行
（真机刷写仅在 Linux 块设备上生效）。

## 调用

```bash
python3 tools/familyos_flash/cli.py --help     # 或 python3 -m familyos_flash（cwd=tools）

# ① 系统镜像写入（先 dry-run 看计划，确认后再 --confirm）
python3 -m familyos_flash flash system.fosimg --to /dev/sdb --confirm --allow-block-device
python3 -m familyos_flash flash system.fosimg --to ./devslot            # 目标为目录=模拟设备
python3 -m familyos_flash flash system.fosimg --to disk.img             # 目标带 .tab 分区表

# ② 数据备份 / 恢复
python3 -m familyos_flash backup --to /dev/sdb --out my-backup --confirm --allow-block-device
python3 -m familyos_flash restore --source my-backup --to /dev/sdb --confirm --allow-block-device

# ③ 环境切换（A/B）
python3 -m familyos_flash slot show --to /dev/sdb
python3 -m familyos_flash slot switch --to /dev/sdb --confirm    # 重启后生效

# ④ 救砖模式：重刷 boot + 回滚槽位 _a
python3 -m familyos_flash rescue --image boot.fosimg --to /dev/sdb --confirm --allow-block-device
```

## 安全设计（本工具的全部意义）

| 层 | 守卫 |
| --- | --- |
| 默认 dry-run | 不 `--confirm` 只打印执行计划，**永不写盘** |
| 块设备双锁 | 真机磁盘需 `--allow-block-device` + `--confirm` |
| root 要求 | 写块设备强制 root（非 root 明确报错，不静默失败） |
| 杂散分区闸 | 目标盘上有内核识别的分区但未声明分区表 → 拒绝整盘操作 |
| 容量检查 | 分区表（`.tab`）声明的容量不足即中止 |
| 写前写后校验 | 镜像头 sha256 + 流式逐块复算；恢复前校验备份自哈希 |
| 中断提示 | Ctrl+C 明确告知用 backup/rescue 修复路径 |

## 测试

```bash
python3 -m unittest discover -s tools/tests -p "test_*.py"
```

24 项测试全部在临时目录用「目录模拟设备」跑通，覆盖镜像格式、守卫矩阵、
A/B 切换、备份恢复往返与 CLI 退出码。

## build-image.sh

`bash tools/build-image.sh --device pc --out out/` 产出可被本工具 flash 的
`.fosimg`：基线 rootfs（debootstrap/multistrap）→ 安装 services 二进制与
systemd 单元 → 打包镜像头（device/part/sha256）。依赖 `rootfs/` 模板目录
（社区贡献各设备的根文件系统配方）。
