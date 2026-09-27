# SPDX-License-Identifier: MulanPSL-2.0
""".fos 包检视（inspect）与完整性校验（verify）。

verify 检查项：
  V1 清单存在且 schema 合法；
  V2 所有条目路径安全（无绝对路径、无 ..、无盘符）；
  V3 entry.path 按每个声明架构解析后真实存在；
  V4 清单为首条目（安装器流式读取的前提）；
  V5 zip 自身 CRC 校验（read test）。
返回 (ok, 问题列表)。
"""

from __future__ import annotations

import zipfile
from typing import Dict, List, Tuple

from .manifest import Manifest, ManifestError, load_manifest_bytes

MANIFEST_ENTRY = "FOS-INF/manifest.json"


class InspectError(Exception):
    """包无法读取（非 zip / 缺清单）。"""


def _path_unsafe(name: str) -> bool:
    return (name.startswith("/") or ".." in name.split("/")
            or ":" in name or name.startswith("\\"))


def inspect_package(path: str) -> Dict:
    """返回摘要 dict：manifest + 条目清单 + 大小。"""
    try:
        zf = zipfile.ZipFile(path)
    except zipfile.BadZipFile as e:
        raise InspectError(f"不是有效 ZIP/.fos: {e}") from e
    with zf:
        if MANIFEST_ENTRY not in zf.namelist():
            raise InspectError(f"缺少 {MANIFEST_ENTRY}")
        with zf.open(MANIFEST_ENTRY) as f:
            man = load_manifest_bytes(f.read())
        infos = zf.infolist()
        return {
            "manifest": man,
            "entries": [{"name": i.filename, "size": i.file_size,
                         "compress": i.compress_type} for i in infos],
            "total_size": sum(i.file_size for i in infos),
        }


def verify_package(path: str) -> Tuple[bool, List[str]]:
    """完整性校验。返回 (是否通过, 问题列表)。"""
    problems: List[str] = []
    try:
        with zipfile.ZipFile(path) as zf:
            names = zf.namelist()
            # V4 首条目
            if not names:
                return False, ["空包"]
            if names[0] != MANIFEST_ENTRY:
                problems.append("清单不是首条目（安装器要求流式首读）")
            # V1 清单
            manifest = None
            if MANIFEST_ENTRY not in names:
                problems.append(f"缺少 {MANIFEST_ENTRY}")
            else:
                with zf.open(MANIFEST_ENTRY) as f:
                    try:
                        manifest = Manifest(load_manifest_bytes(f.read()))
                    except ManifestError as e:
                        problems.extend(f"清单: {m}" for m in e.errors)
            # V2 路径安全
            for n in names:
                if _path_unsafe(n):
                    problems.append(f"危险条目路径: {n!r}")
            # V5 zip CRC
            bad = zf.testzip()
            if bad:
                problems.append(f"zip CRC 失败: {bad}")
            # V3 入口存在
            if manifest:
                sset = set(names)
                for arch in manifest.arch:
                    want = manifest.resolved_entry(arch)
                    if want not in sset:
                        problems.append(
                            f"架构 {arch} 入口缺失: {want}")
    except zipfile.BadZipFile as e:
        return False, [f"不是有效 ZIP/.fos: {e}"]
    return (not problems), problems
