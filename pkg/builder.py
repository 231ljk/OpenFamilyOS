# SPDX-License-Identifier: MIT
"""把「源目录 + manifest.json」构建为 .fos 包。

构建规则（对应 docs/fos-package-format.md）：
  1. manifest.json 必须位于源目录根部，校验通过才继续；
  2. ZIP 首条目固定为 FOS-INF/manifest.json（store 不压缩，便于流式读取）；
  3. 源目录按子树映射：bin/ lib/ res/ data/ 原名保留，其余顶层文件拒绝；
  4. entry.path 解析后（替换 {arch}）必须在包内真实存在，逐架构校验；
  5. 产物按固定顺序写入（清单 → 其余文件按名排序）；清单条目用
     固定时间戳，保证清单本身字节可复现。
"""

from __future__ import annotations

import json
import os
import zipfile
from typing import List, Optional, Tuple

from .manifest import Manifest, load_manifest_bytes

ALLOWED_ROOT_DIRS = {"bin", "lib", "res", "data"}
MANIFEST_ENTRY = "FOS-INF/manifest.json"


class BuildError(Exception):
    """构建输入不合法。"""


def _collect_files(src_dir: str) -> List[Tuple[str, str]]:
    """枚举源目录 → [(归档名, 磁盘路径)]；根目录只许放 manifest.json。"""
    out: List[Tuple[str, str]] = []
    for root, dirs, files in os.walk(src_dir):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        rel_root = os.path.relpath(root, src_dir).replace("\\", "/")
        top = rel_root.split("/", 1)[0] if rel_root != "." else "."
        if top == ".":
            for fn in files:
                if fn != "manifest.json":
                    raise BuildError(
                        f"源目录根部只允许 manifest.json，多余: {fn}")
            continue
        if top not in ALLOWED_ROOT_DIRS:
            raise BuildError(
                f"未知顶层目录 {top}/（允许 {sorted(ALLOWED_ROOT_DIRS)}）")
        for fn in sorted(files):
            if fn.startswith("."):
                continue
            disk = os.path.join(root, fn)
            arc = os.path.relpath(disk, src_dir).replace("\\", "/")
            out.append((arc, disk))
    return out


def build_package(src_dir: str, out_path: str,
                  extra_info: Optional[dict] = None) -> Manifest:
    """构建 .fos。返回生效的 Manifest。

    extra_info: 转包引擎注入 FOS-INF/converter.info 的内容。
    """
    mpath = os.path.join(src_dir, "manifest.json")
    if not os.path.isfile(mpath):
        raise BuildError(f"缺少 {mpath}")
    with open(mpath, "rb") as f:
        man = load_manifest_bytes(f.read())
    manifest = Manifest(man)

    files = _collect_files(src_dir)

    # entry 逐架构必须存在
    arcs = {a for a, _ in files}
    for arch in manifest.arch:
        want = manifest.resolved_entry(arch)
        if want not in arcs:
            raise BuildError(
                f"架构 {arch} 的入口不存在: {want}（检查 entry.path 的 "
                f"{{arch}} 与 bin/{arch}/ 布局）")

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        # 首条目：清单，store；字节序固定（sort_keys + 末尾换行 + 定时间戳）
        zi = zipfile.ZipInfo(MANIFEST_ENTRY, date_time=(1980, 1, 1, 0, 0, 0))
        zi.compress_type = zipfile.ZIP_STORED
        zf.writestr(zi, manifest.to_json_bytes())
        if extra_info:
            zf.writestr("FOS-INF/converter.info",
                        json.dumps(extra_info, ensure_ascii=False,
                                   indent=2, sort_keys=True) + "\n")
        for arc, disk in sorted(files):
            if arc == "manifest.json":
                continue      # 根清单不重复写入
            zf.write(disk, arc)
    return manifest
