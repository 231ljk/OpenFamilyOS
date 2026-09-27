# SPDX-License-Identifier: MulanPSL-2.0
"""转包引擎：APK / deb / rpm → .fos。

策略（对应开发文档「主流应用官方适配优先，小厂通过转包引擎兼容，
运行在沙箱中」）：

  入口提取          运行时布局
  ─────────         ─────────────────────────────
  APK  lib/<abi>/   bin/<arch>/ + data/rootfs/ 整包透传
       assets/bin   （沙箱以 rootfs 为只读根，entry 直跑）
  deb  usr/bin/*    bin/<arch>/ + data/rootfs/
  rpm  usr/bin/*    同上（需系统 rpm2cpio，缺失则明确报错）

abi/arch 映射：
  armeabi-v7a / arm64-v8a → arm64（v7a 在 ARM64 上经内核兼容运行）
  x86_64 / amd64          → x86_64（虚拟化兼容层翻译承接）
  i386                    → x86_64（同上，32 位子集）

转包不翻译指令、不重写代码 —— 只做「清单化 + 装箱」。运行时由
virt-compat（窗口化/全屏由设备形态决定）与沙箱承接。
"""

from __future__ import annotations

import io
import json
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile
from typing import Dict, Optional, Tuple

from .builder import BuildError, build_package

# 外部架构 → FamilyOS arch
ARCH_MAP = {
    "arm64-v8a": "arm64",
    "armeabi-v7a": "arm64",
    "armeabi": "arm64",
    "arm64": "arm64",
    "aarch64": "arm64",
    "x86_64": "x86_64",
    "amd64": "x86_64",
    "i386": "x86_64",
    "i686": "x86_64",
}

ENGINE_VERSION = "fos-convert/0.1"


class ConvertError(Exception):
    """源包无法转换。"""


def detect_source(path: str) -> str:
    """识别源格式：apk / deb / rpm / fos。"""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".fos":
        return "fos"
    if ext in (".apk", ".apk.3p"):
        return "apk"
    if ext == ".deb":
        return "deb"
    if ext in (".rpm",):
        return "rpm"
    # 内容嗅探兜底
    try:
        with open(path, "rb") as f:
            head = f.read(8)
    except OSError:
        return "unknown"
    if head[:4] == b"PK\x03\x04":
        return "apk"          # zip 容器：按 apk 尽力解析
    if head[:8] == b"!<arch>\n":
        return "deb"
    if head[:4] == b"\xed\xab\xee\xdb":
        return "rpm"
    return "unknown"


# ───────────────────────── APK ─────────────────────────

def _convert_apk(src: str, stage: str) -> Dict:
    """从 APK（zip）提取可运行负载与元数据。

    不解析二进制 AndroidManifest（那需要 aapt/axml 解码器，留给社区
    适配器）。清单字段提取顺序：
      id   ← zip 内包名线索（META-INF 注释 / assets/app.id）或文件名
      entry ← lib/<abi>/*.so 与 assets/bin/*（可执行负载）
    """
    try:
        zf = zipfile.ZipFile(src)
    except zipfile.BadZipFile as e:
        raise ConvertError(f"APK 不是有效 zip: {e}") from e

    picked: Dict[str, str] = {}        # fos 归档名 → 磁盘相对路径
    archs: set = set()
    with zf:
        names = zf.namelist()
        for n in names:
            m = re.match(r"^lib/([^/]+)/(.+\.so)$", n)
            if m:
                ext_arch = m.group(1)
                if ext_arch not in ARCH_MAP:
                    continue
                arch = ARCH_MAP[ext_arch]
                fname = os.path.basename(m.group(2))
                arc = f"bin/{arch}/{fname}"
                _extract(zf, n, os.path.join(stage, arc))
                picked[arc] = n
                archs.add(arch)
                continue
            m = re.match(r"^assets/bin/([^/]+)/(.+)$", n)
            if m and not n.endswith("/"):
                ext_arch = m.group(1)
                if ext_arch not in ARCH_MAP:
                    continue
                arch = ARCH_MAP[ext_arch]
                fname = m.group(2)
                arc = f"bin/{arch}/{fname}"
                _extract(zf, n, os.path.join(stage, arc))
                picked[arc] = n
                archs.add(arch)
                continue
            if n.startswith(("res/", "assets/", "lib/")) or \
               n in ("AndroidManifest.xml", "classes.dex"):
                arc = "data/rootfs/" + n
                _extract(zf, n, os.path.join(stage, arc))
        pkg = _guess_apk_id(zf, src, names)
        ver = _guess_apk_version(zf, names)

    if not picked:
        raise ConvertError(
            "APK 内未找到可执行负载（lib/<abi>/*.so 或 assets/bin/*）。"
            "纯 Java/Kotlin 应用请走官方适配（ART 运行时），转包引擎"
            "面向带原生二进制的负载。")
    if not archs:
        raise ConvertError("无受支持架构")
    entry_arc = sorted(picked)[0]
    return {
        "id": pkg,
        "archs": sorted(archs),
        "entry": entry_arc,
        "version_name": ver,
        "source": {"kind": "apk", "entries": picked},
    }


def _guess_apk_id(zf: zipfile.ZipFile, src: str, names) -> str:
    # 1) assets/app.id（我们约定的旁路元数据）
    if "assets/app.id" in names:
        raw = zf.read("assets/app.id").decode("utf-8", "replace").strip()
        if re.match(r"^[a-z0-9_]+(\.[a-z0-9_]+)+$", raw):
            return raw
    # 2) zip 注释（部分打包工具写入包名）
    try:
        cmt = zf.comment.decode("utf-8", "ignore")
        m = re.search(r"package=([a-zA-Z0-9_.]+)", cmt)
        if m:
            return m.group(1).lower()
    except Exception:
        pass
    # 3) 文件名兜底
    base = os.path.splitext(os.path.basename(src))[0]
    base = re.sub(r"[^a-zA-Z0-9_.]", "_", base).lower().strip("_")
    if not base or not re.match(r"^[a-z0-9_]+(\.[a-z0-9_]+)+$", base):
        base = "converted." + (base or "app")
        base = re.sub(r"\.+", ".", base)
    return base


def _guess_apk_version(zf: zipfile.ZipFile, names) -> str:
    if "assets/app.version" in names:
        v = zf.read("assets/app.version").decode("utf-8", "replace").strip()
        if v:
            return v[:24]
    return "0.0.0"


def _extract(zf: zipfile.ZipFile, arcname: str, dest: str) -> None:
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with zf.open(arcname) as src_f, open(dest, "wb") as dst_f:
        shutil.copyfileobj(src_f, dst_f)
    os.chmod(dest, os.stat(dest).st_mode | 0o111)


# ───────────────────────── deb ─────────────────────────

def _read_ar(path: str) -> Dict[str, bytes]:
    """解析 deb 的 ar 容器：!<arch>\\n + 60B 头 + 数据。"""
    members: Dict[str, bytes] = {}
    with open(path, "rb") as f:
        if f.read(8) != b"!<arch>\n":
            raise ConvertError("非 ar 容器（不是有效 .deb）")
        while True:
            hdr = f.read(60)
            if len(hdr) < 60:
                break
            name = hdr[0:16].decode("ascii", "replace").strip().rstrip("/")
            try:
                size = int(hdr[48:58].decode().strip())
            except ValueError:
                raise ConvertError(f"ar 成员长度非法: {name}") from None
            data = f.read(size)
            if len(data) < size:
                raise ConvertError(f"ar 成员截断: {name}")
            members[name] = data
            if size % 2:
                f.read(1)   # ar 偶数对齐
    return members


def _convert_deb(src: str, stage: str) -> Dict:
    ar = _read_ar(src)
    control: Dict[str, str] = {}
    control_raw = None
    data_files: Dict[str, bytes] = {}

    for name, blob in ar.items():
        if name.startswith("control.tar"):
            with tarfile.open(fileobj=io.BytesIO(blob)) as tf:
                for ti in tf.getmembers():
                    if ti.name.lstrip("./") == "control" and ti.isfile():
                        fh = tf.extractfile(ti)
                        control_raw = fh.read().decode("utf-8", "replace")
        elif name.startswith("data.tar"):
            with tarfile.open(fileobj=io.BytesIO(blob)) as tf:
                for ti in tf.getmembers():
                    if not ti.isfile():
                        continue
                    rel = ti.name.lstrip("./")
                    if ti.size > 256 * 1024 * 1024:
                        continue   # 超大文件拒绝内嵌（交给社区流式适配器）
                    data_files[rel] = tf.extractfile(ti).read()

    if control_raw is None:
        raise ConvertError("deb 缺 control 成员")
    for line in control_raw.splitlines():
        if ":" in line:
            k, _, v = line.partition(":")
            control[k.strip().lower()] = v.strip()

    pkg = control.get("package", "").lower().strip()
    pkg = re.sub(r"[^a-z0-9_.+-]", "_", pkg) or "converted.app"
    if "." not in pkg:
        pkg = "converted." + pkg
    arch_raw = control.get("architecture", "all")
    arch = ARCH_MAP.get(arch_raw, "x86_64" if arch_raw == "all" else None)
    if arch is None:
        raise ConvertError(f"deb 架构不支持: {arch_raw}")

    # 入口：usr/bin/* 第一个可执行
    entry_rel = None
    for rel in sorted(data_files):
        if rel.startswith("usr/bin/") or rel.startswith("usr/local/bin/"):
            entry_rel = rel
            break
    if entry_rel is None:
        raise ConvertError("deb 未找到 usr/bin 入口（数据包装配型请官方适配）")

    arc_entry = f"bin/{arch}/{os.path.basename(entry_rel)}"
    dest = os.path.join(stage, arc_entry.replace("/", os.sep))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "wb") as f:
        f.write(data_files[entry_rel])
    os.chmod(dest, 0o755)

    # 其余文件透传为沙箱 rootfs
    for rel, blob in data_files.items():
        d = os.path.join(stage, "data", "rootfs",
                         rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(d), exist_ok=True)
        with open(d, "wb") as f:
            f.write(blob)

    ver = control.get("version", "0.0.0")
    ver = re.sub(r"^(\d+:)?", "", ver)[:24]
    return {
        "id": pkg,
        "archs": [arch],
        "entry": arc_entry,
        "version_name": ver,
        "source": {"kind": "deb", "control": {
            "package": pkg, "version": ver, "architecture": arch_raw}},
    }


# ───────────────────────── rpm ─────────────────────────

def _convert_rpm(src: str, stage: str) -> Dict:
    """rpm：需要系统 rpm2cpio（格式含 header 索引，纯 Python 重写不划算）。

    缺失时明确报错并给出替代路径 —— 诚实比假装能干更重要。
    """
    if shutil.which("rpm2cpio") is None:
        raise ConvertError(
            "转包 rpm 需要系统工具 rpm2cpio（dnf/apt 安装 rpm2cpio），"
            "或将 rpm 先转 cpio 后使用 convert --source-cpio。")
    proc = subprocess.run(["rpm2cpio", src], stdout=subprocess.PIPE,
                          check=True)
    files: Dict[str, bytes] = {}
    with tarfile.open(fileobj=io.BytesIO(proc.stdout)) as tf:
        for ti in tf.getmembers():
            if not ti.isfile():
                continue
            rel = re.sub(r"^\./", "", ti.name)
            files[rel] = tf.extractfile(ti).read()

    entry_rel = next((r for r in sorted(files)
                      if r.startswith("usr/bin/")), None)
    if entry_rel is None:
        raise ConvertError("rpm 未找到 usr/bin 入口")
    arch = "x86_64"   # rpm2cpio 不导出架构；保守交给翻译层
    arc_entry = f"bin/{arch}/{os.path.basename(entry_rel)}"
    dest = os.path.join(stage, arc_entry.replace("/", os.sep))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "wb") as f:
        f.write(files[entry_rel])
    os.chmod(dest, 0o755)
    for rel, blob in files.items():
        d = os.path.join(stage, "data", "rootfs",
                         rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(d), exist_ok=True)
        with open(d, "wb") as f:
            f.write(blob)

    base = os.path.splitext(os.path.basename(src))[0]
    pkg = re.sub(r"[^a-z0-9_.]", "_", base).lower()
    m = re.search(r"-(\d[^-]*)$", base)
    ver = m.group(1) if m else "0.0.0"
    return {
        "id": pkg if "." in pkg else "converted." + pkg,
        "archs": [arch],
        "entry": arc_entry,
        "version_name": ver,
        "source": {"kind": "rpm", "note": "经 rpm2cpio 解包"},
    }


# ───────────────────────── 总入口 ─────────────────────────

_CONVERTERS = {"apk": _convert_apk, "deb": _convert_deb, "rpm": _convert_rpm}


def convert_package(src: str, out_path: str,
                    manifest_override: Optional[Dict] = None) -> Dict:
    """转换外部包为 .fos。返回 {id, entry, archs, out}。

    manifest_override 允许覆盖/补充字段（如官方要求重新署名 id）。
    """
    kind = detect_source(src)
    if kind == "fos":
        raise ConvertError("源已是 .fos，无需转换")
    if kind not in _CONVERTERS:
        raise ConvertError(f"无法识别源格式: {src}")

    stage = tempfile.mkdtemp(prefix="fosconv-")
    try:
        info = _CONVERTERS[kind](src, stage)
        entry_file = os.path.join(stage,
                                  info["entry"].replace("/", os.sep))
        with open(entry_file, "rb") as f:
            magic = f.read(4)
        if magic[:4] != b"\x7fELF" and kind != "apk":
            raise ConvertError(f"入口不是 ELF 可执行: {info['entry']}")

        man: Dict = {
            "format": 1,
            "id": info["id"],
            "name": info["id"].split(".")[-1].replace("_", "-"),
            "version_code": 1,
            "version_name": info["version_name"],
            "min_os": "0.1.0",
            "arch": info["archs"],
            "entry": {"type": "bridge", "path": info["entry"]},
            "permissions": [],
            "sandbox": {"mode": "strict", "fs_quota_mb": 512},
        }
        if manifest_override:
            man.update(manifest_override)

        # 清单落盘进暂存目录，build_package 从根部读取
        with open(os.path.join(stage, "manifest.json"), "wb") as f:
            f.write((json.dumps(man, ensure_ascii=False, indent=2,
                                sort_keys=True) + "\n").encode("utf-8"))

        converter_info = {
            "engine": ENGINE_VERSION,
            "from": kind,
            **info["source"],
        }
        build_package(stage, out_path, extra_info=converter_info)
        return {"id": man["id"], "entry": man["entry"]["path"],
                "archs": man["arch"], "out": out_path}
    except BuildError as e:
        raise ConvertError(str(e)) from e
    finally:
        shutil.rmtree(stage, ignore_errors=True)
