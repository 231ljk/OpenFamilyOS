# SPDX-License-Identifier: MIT
"""FamilyOS 统一刷机与维护工具。

覆盖开发文档第八章的四项能力：
    flash   系统镜像写入
    backup  数据备份 / restore 恢复
    slot    A/B 环境切换
    rescue  救砖模式

安全默认：**所有真机写操作 --dry-run 为默认**，必须显式 --confirm
才落盘 —— 防止一条命令误刷设备。本模块不 import 任何硬件，
设备访问集中在 device.py，便于社区为不同设备形态适配。
"""

from .image import (ImageError, ImageHeader, load_header, make_placeholder,
                    sha256_file, write_stream)
from .device import Device, DeviceError, enumerate_targets, resolve_target
from .backup import (BackupError, create_backup, list_partitions,
                     restore_backup)
from .slot import SlotError, get_current_slot, set_active_slot, switch_slot

__all__ = [
    "ImageHeader", "ImageError", "load_header", "make_placeholder",
    "sha256_file", "write_stream",
    "Device", "DeviceError", "enumerate_targets", "resolve_target",
    "BackupError", "create_backup", "restore_backup", "list_partitions",
    "SlotError", "get_current_slot", "set_active_slot", "switch_slot",
]
