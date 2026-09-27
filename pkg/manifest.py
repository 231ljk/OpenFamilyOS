# SPDX-License-Identifier: MulanPSL-2.0
"""manifest.json 的 schema 校验与读取（对应 docs/fos-package-format.md §2）。"""

from __future__ import annotations

import json
import re
from typing import Any, Dict, List

VALID_ARCH = {"arm64", "x86_64"}
VALID_ENTRY_TYPES = {"binary", "bridge"}
VALID_SANDBOX = {"strict", "legacy"}

_ID_RE = re.compile(r"^[a-z0-9_]+(\.[a-z0-9_]+)+$")
_NAME_RE = re.compile(r"^[A-Za-z0-9_\-\u4e00-\u9fff ]{1,48}$")


class ManifestError(Exception):
    """清单字段校验失败。"""

    def __init__(self, errors: List[str]):
        self.errors = errors
        super().__init__("; ".join(errors))


def _must(man: Dict[str, Any], errs: List[str], key: str,
          typ: type) -> None:
    """必填字段存在性 + 类型检查（bool 不算 int）。"""
    if key not in man:
        errs.append(f"缺少必填字段: {key}")
        return
    v = man[key]
    if typ is int and isinstance(v, bool):
        errs.append(f"{key} 必须是整数")
    elif not isinstance(v, typ):
        errs.append(f"{key} 类型应为 {typ.__name__}，实际 {type(v).__name__}")


def validate_manifest(man: Dict[str, Any]) -> List[str]:
    """返回错误列表；空列表 = 合法。逐字段校验，可一次报全。"""
    errs: List[str] = []

    fmt = man.get("format")
    if fmt != 1:
        errs.append(f"format 必须为 1，当前 {fmt!r}（v1 解析器）")

    _must(man, errs, "id", str)
    _must(man, errs, "name", str)
    _must(man, errs, "version_code", int)
    _must(man, errs, "version_name", str)

    idv = man.get("id", "")
    if isinstance(idv, str) and idv and not _ID_RE.match(idv):
        errs.append(f"id 不符合反向域名规则: {idv!r}")
    nv = man.get("name", "")
    if isinstance(nv, str) and nv and not _NAME_RE.match(nv):
        errs.append(f"name 含非法字符: {nv!r}")
    vc = man.get("version_code")
    if isinstance(vc, int) and vc < 1:
        errs.append("version_code 必须 >= 1")

    arch = man.get("arch")
    if not isinstance(arch, list) or not arch:
        errs.append("arch 必须是非空数组")
    else:
        bad = [a for a in arch if a not in VALID_ARCH]
        if bad:
            errs.append(f"未知架构 {bad}（支持 {sorted(VALID_ARCH)}）")

    entry = man.get("entry")
    if not isinstance(entry, dict):
        errs.append("entry 必须是对象")
    else:
        et = entry.get("type", "binary")
        if et not in VALID_ENTRY_TYPES:
            errs.append(f"entry.type 非法: {et!r}")
        ep = entry.get("path")
        if not isinstance(ep, str) or not ep:
            errs.append("entry.path 必填")
        elif ".." in ep or ep.startswith("/"):
            errs.append(f"entry.path 越界: {ep!r}")

    sb = man.get("sandbox")
    if isinstance(sb, dict):
        if sb.get("mode", "strict") not in VALID_SANDBOX:
            errs.append(f"sandbox.mode 非法: {sb['mode']!r}")
        q = sb.get("fs_quota_mb", 128)
        if not isinstance(q, int) or q <= 0 or q > 65536:
            errs.append("sandbox.fs_quota_mb 必须在 (0, 65536]")
    elif sb is not None:
        errs.append("sandbox 必须是对象")

    perms = man.get("permissions", [])
    if not isinstance(perms, list) or any(not isinstance(p, str) for p in perms):
        errs.append("permissions 必须是字符串数组")

    up = man.get("upgrade_from", [])
    if not isinstance(up, list) or any(not isinstance(u, str) for u in up):
        errs.append("upgrade_from 必须是字符串数组")

    return errs


def load_manifest_bytes(data: bytes) -> Dict[str, Any]:
    """从 manifest.json 字节流读取并校验；失败抛 ManifestError。"""
    try:
        man = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise ManifestError([f"manifest.json 无法解析: {e}"]) from e
    if not isinstance(man, dict):
        raise ManifestError(["manifest.json 顶层必须是对象"])
    errs = validate_manifest(man)
    if errs:
        raise ManifestError(errs)
    return man


class Manifest(dict):
    """校验通过的 manifest 字典；属性访问便捷字段。

    用法::

        Manifest.load(zipobj)      # 从 .fos 读
        Manifest.from_dict(man)    # 已校验的 dict
    """

    def __init__(self, man: Dict[str, Any]):
        errs = validate_manifest(man)
        if errs:
            raise ManifestError(errs)
        super().__init__(man)

    @classmethod
    def from_dict(cls, man: Dict[str, Any]) -> "Manifest":
        return cls(man)

    @classmethod
    def load(cls, zf) -> "Manifest":
        """从 zipfile 读取首清单条目。zf 为已打开的 zipfile.ZipFile。"""
        import zipfile

        if "FOS-INF/manifest.json" not in zf.namelist():
            raise ManifestError(["缺少 FOS-INF/manifest.json"])
        with zf.open("FOS-INF/manifest.json") as f:
            return cls(load_manifest_bytes(f.read()))

    # 便捷字段
    @property
    def id(self) -> str:
        return self["id"]

    @property
    def name(self) -> str:
        return self["name"]

    @property
    def version_code(self) -> int:
        return self["version_code"]

    @property
    def version_name(self) -> str:
        return self["version_name"]

    @property
    def arch(self) -> List[str]:
        return list(self["arch"])

    @property
    def entry_path(self) -> str:
        return self["entry"]["path"]

    def resolved_entry(self, arch: str) -> str:
        """把 entry.path 里的 {arch} 占位替换为实际架构。"""
        return self.entry_path.replace("{arch}", arch)

    def to_json_bytes(self) -> bytes:
        return (json.dumps(self, ensure_ascii=False, indent=2,
                           sort_keys=True) + "\n").encode("utf-8")
