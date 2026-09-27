# SPDX-License-Identifier: MulanPSL-2.0
"""分区备份 / 恢复。

备份格式（目录形态，可读、可选件、适配超级环互传）：

    backup-<device>-<ts>/
    ├── meta.json         目标、分区、大小、哈希、恢复顺序
    └── <分区名>.img      逐分区 raw（可单独取用）

restore 先校验 meta 与逐分区 sha256，再询问目标分区存在性；
默认 dry-run 列计划，--confirm 才写。
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import time
from typing import List, Optional

from .device import Device, DeviceError
from .image import ImageError

_CHUNK = 1024 * 1024
META = "meta.json"


class BackupError(Exception):
    """备份/恢复失败。"""


def list_partitions(dev: Device) -> List[str]:
    """可备份分区 = 分区表声明 > 目录设备动态发现的 *.img > ['whole']。"""
    if dev.parts:
        return [p.name for p in dev.parts]
    if dev.kind == "dir":
        try:
            found = sorted(fn[:-4] for fn in os.listdir(dev.path)
                           if fn.endswith(".img"))
            if found:
                return found
        except OSError:
            pass
    return ["whole"]


def _sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(_CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def create_backup(dev: Device, out_dir: str,
                  partitions: Optional[List[str]] = None,
                  *, confirm: bool = False) -> dict:
    """备份分区到目录。返回 meta。"""
    if not confirm:
        raise BackupError("未执行：请加 --confirm（备份会向 out 目录写盘）。")
    if os.path.exists(out_dir):
        raise BackupError(f"输出目录已存在: {out_dir}")
    if dev.kind == "file" and not os.path.exists(dev.path):
        raise BackupError(f"目标不存在，无内容可备份: {dev.path}")

    parts = partitions or list_partitions(dev)
    known = list_partitions(dev)
    for p in parts:
        if p not in known:
            raise BackupError(f"目标无分区 {p}（已知 {known}）")

    os.makedirs(out_dir)
    meta = {"tool": "familyos_flash", "format": 1,
            "device": dev.device_id, "target": dev.path,
            "created": int(time.time()),
            "partitions": []}
    try:
        for name in parts:
            fh, off, _cap = dev.open(name, "rb")
            fn = os.path.join(out_dir, name + ".img")
            total = 0
            try:
                fh.seek(off)
                h = hashlib.sha256()
                with open(fn, "wb") as out:
                    while True:
                        buf = fh.read(_CHUNK)
                        if not buf:
                            break
                        out.write(buf)
                        h.update(buf)
                        total += len(buf)
                meta["partitions"].append(
                    {"name": name, "file": name + ".img",
                     "size": total, "sha256": h.hexdigest()})
            finally:
                fh.close()
    except Exception:
        shutil.rmtree(out_dir, ignore_errors=True)
        raise
    with open(os.path.join(out_dir, META), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2, sort_keys=True)
    return meta


def _check_meta(path: str) -> dict:
    mp = os.path.join(path, META)
    if not os.path.isfile(mp):
        raise BackupError(f"{path} 不是备份目录（缺 {META}）")
    with open(mp, encoding="utf-8") as f:
        meta = json.load(f)
    if meta.get("format") != 1:
        raise BackupError(f"备份格式版本不支持: {meta.get('format')}")
    return meta


def restore_backup(dev: Device, from_dir: str,
                   partitions: Optional[List[str]] = None,
                   *, confirm: bool = False) -> List[str]:
    """从备份恢复。默认恢复 meta 里全部已声明分区。

    返回实际恢复（或 dry-run 计划）的分区名列表。
    校验链：meta → 源文件哈希 → 分区声明 → 写入。
    """
    meta = _check_meta(from_dir)
    plan = [p for p in meta["partitions"]
            if not partitions or p["name"] in partitions]
    if partitions:
        want = {p["name"] for p in plan}
        missing = set(partitions) - want
        if missing:
            raise BackupError(f"备份缺少分区: {sorted(missing)}")

    known = list_partitions(dev)
    for p in plan:                      # 校验阶段：先全部检查再动笔
        src = os.path.join(from_dir, p["file"])
        if not os.path.isfile(src):
            raise BackupError(f"备份缺文件: {p['file']}")
        if _sha256_of(src) != p["sha256"]:
            raise BackupError(f"备份文件损坏，拒绝恢复: {p['file']}")
        if p["name"] not in known:
            raise BackupError(
                f"目标无分区 {p['name']}（已知 {known}）")

    if not confirm:
        return [p["name"] for p in plan]        # dry-run：仅回计划

    done = []
    for p in plan:
        src = os.path.join(from_dir, p["file"])
        if dev.kind == "dir":
            # 覆盖写残留问题：目录模拟设备先把分区文件截到备份大小。
            # 真机块设备由 bootloader 按 meta 声明的 size 校验，无需截断。
            fn = os.path.join(dev.path, p["name"] + ".img")
            if os.path.exists(fn):
                os.truncate(fn, p["size"])
        fh, off, _cap = dev.open(p["name"], "r+b")
        try:
            fh.seek(off)
            with open(src, "rb") as f:
                shutil.copyfileobj(f, fh, _CHUNK)
            fh.flush()
            os.fsync(fh.fileno())
        finally:
            fh.close()
        done.append(p["name"])
    return done
