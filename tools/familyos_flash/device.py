# SPDX-License-Identifier: MIT
"""写入目标抽象：真实块设备 / 回环镜像文件 / 目录槽位模拟。

安全红线（本工具存在的全部意义之一）：
  1. 真机块设备（/dev/nvme*、/dev/mmcblk*、/dev/sd*）默认 **拒绝直接写**；
     必须 --confirm 且显式 --allow-block-device 双开关；
  2. 写块设备前强制只读探测分区表，若目标上存在未在原设备表声明的
     分区（可能是用户数据盘），中止；
  3. 所有真机写操作要求 root（euid==0），非 root 直接失败并给指引。

设备矩阵见 devices/README.md；本模块只认「目标路径 + 分区表声明」。
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from .image import ImageError

# 常见真实磁盘前缀（Linux）。命中即视为块设备目标。
_BLOCK_RE = re.compile(r"^/dev/(nvme\d+n\d+|mmcblk\d+|sd[a-z]+|vd[a-z]+)$")


class DeviceError(Exception):
    """目标不可用/不安全。"""


@dataclass
class Partition:
    name: str            # system_a / boot_a / userdata …
    offset: int          # 字节偏移
    size: int            # 字节容量


@dataclass
class Device:
    """一个可刷写目标。

    kind:
      block   真实块设备（受最严守卫）
      file    镜像文件（回环开发用；等同虚拟设备）
      dir     目录槽位（A/B 环境切换的模拟实现）
    """
    path: str
    kind: str = "file"
    device_id: str = "generic"
    slots: List[str] = field(default_factory=lambda: ["_a", "_b"])
    parts: List[Partition] = field(default_factory=list)

    # ── 探测 ──

    @staticmethod
    def probe(path: str) -> "Device":
        """按现状探测目标类型。"""
        if _BLOCK_RE.match(path):
            return Device(path=path, kind="block")
        if os.path.isdir(path):
            dev = Device(path=path, kind="dir")
            tab = os.path.join(path, "flash.tab")
            if os.path.isfile(tab):
                dev.parts = _read_tab(tab)
            return dev
        if os.path.exists(path):
            dev = Device(path=path, kind="file")
            # 镜像文件内嵌分区表声明：同名 .tab 文件（"名称 偏移 大小" 每行）
            tab = path + ".tab"
            if os.path.isfile(tab):
                dev.parts = _read_tab(tab)
            return dev
        # 不存在：当作待创建的镜像文件（由上层决定 create）
        return Device(path=path, kind="file")

    def partition(self, name: str) -> Partition:
        for p in self.parts:
            if p.name == name:
                return p
        raise DeviceError(
            f"目标 {self.path} 未声明分区 {name!r}"
            f"（已知: {[p.name for p in self.parts] or '无分区表'}）")

    def capacity(self) -> int:
        if self.kind == "file":
            return os.path.getsize(self.path) if os.path.exists(self.path) else 0
        if self.kind == "block":
            # 真机：从 sysfs 读 size（512B 扇区计数）
            try:
                with open(self.path.replace("/dev/",
                              "/sys/block/") + "/size") as f:
                    return int(f.read().strip()) * 512
            except OSError:
                return 0
        return 0

    # ── 守卫 ──

    def guard_write(self, partition: str, size: int, *,
                    allow_block: bool = False,
                    confirm: bool = False) -> None:
        """任何真机写入前必须过这一关。不通过则抛 DeviceError。"""
        if self.kind == "block" and not allow_block:
            raise DeviceError(
                f"目标 {self.path} 是真实块设备。此操作会破坏磁盘数据。\n"
                "  确认风险后加 --allow-block-device 重跑。")
        if not confirm:
            raise DeviceError(
                "未确认执行：请加 --confirm（当前为 dry-run 保护默认）。")
        if self.kind == "block":
            if getattr(os, "geteuid", lambda: 0)() != 0:
                raise DeviceError("写块设备需要 root：sudo 重跑本命令。")
            self._check_stray_partitions()
        if self.parts:
            p = self.partition(partition)
            if size > p.size:
                raise ImageError(
                    f"分区 {partition} 容量 {p.size} < 写入 {size}，中止")
        elif self.kind == "file":
            cap = self.capacity()
            if cap and size > cap:
                raise ImageError(f"目标镜像剩余空间不足（{cap} 字节）")

    def _check_stray_partitions(self) -> None:
        """块设备上若已有内核识别出的分区，而设备表未声明它们 → 中止。

        这是防「把带用户数据的盘当裸设备刷了」的最后一道闸。
        """
        try:
            base = os.path.basename(self.path)
            names = {e for e in os.listdir("/sys/class/block")
                     if e.startswith(base) and e != base}
        except OSError:
            return
        if names and not self.parts:
            raise DeviceError(
                f"{self.path} 上检测到内核识别的分区 {sorted(names)}，"
                "但目标未声明分区表（.tab）。拒绝整盘操作，请先声明分区布局。")

    # ── 打开 ──

    def open(self, partition: str, mode: str = "r+b"):
        """返回可 seek 的写入流与 (offset, size)。dir 模拟返回文件句柄。"""
        if self.kind == "dir":
            fn = os.path.join(self.path, partition + ".img")
            if not os.path.exists(fn) and ("w" in mode or "+" in mode):
                open(fn, "wb").close()
            return open(fn, mode), 0, os.path.getsize(fn)
        p = self.partition(partition) if self.parts else None
        if p is None:
            if self.kind == "file" and mode == "wb":
                with open(self.path, "wb"):
                    pass
            return open(self.path, mode), 0, self.capacity()
        return open(self.path, mode), p.offset, p.size


def _read_tab(path: str) -> List[Partition]:
    parts = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            cols = line.split()
            if len(cols) != 3:
                raise DeviceError(f"分区表行格式错误: {line!r}")
            parts.append(Partition(cols[0], int(cols[1]), int(cols[2])))
    return parts


def enumerate_targets(devices_dir: str = "devices") -> List[Dict]:
    """列出受支持设备（读 devices/*/flash.yaml 的极简解析，不依赖 pyyaml）。"""
    out = []
    if not os.path.isdir(devices_dir):
        return out
    for name in sorted(os.listdir(devices_dir)):
        cfg = os.path.join(devices_dir, name, "flash.yaml")
        if os.path.isfile(cfg):
            info = {"device": name}
            with open(cfg, encoding="utf-8") as f:
                for line in f:
                    if ":" in line and not line.strip().startswith("#"):
                        k, _, v = line.partition(":")
                        info[k.strip()] = v.strip()
            out.append(info)
    return out


def resolve_target(path: str, device_id: Optional[str] = None) -> Device:
    """CLI 入口：探测 + 绑定设备 id。"""
    dev = Device.probe(path)
    if device_id:
        dev.device_id = device_id
    return dev
