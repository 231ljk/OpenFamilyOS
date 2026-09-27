# SPDX-License-Identifier: MulanPSL-2.0
"""系统镜像（.fosimg）格式与流式写入辅助。

镜像头 4096 字节（对齐常见擦除粒度），小端：

    magic      8s   "FOSIMG01"
    version    I    = 1
    device     32s  目标设备标识（pc/tablet/phone/tv/watch 或板级 id）
    part       24s  目标分区名（system_a / boot_a / …）
    kind       16s  raw | sparse | ext4 | erofs
    size       Q    有效负载字节数
    sha256     32s  负载哈希（写入时边写边算，写完复核）
    seq        I    镜像序号（同一分区多版本排序）
    created_ms Q    构建时间戳
    reserved   …    填充到 4096

写入策略：先校验头 → 流式搬运 → 逐块计算 sha256 → 收尾比对。
全程不把整镜像读进内存（大镜像友好）。
"""

from __future__ import annotations

import hashlib
import os
import struct
from dataclasses import dataclass
from typing import BinaryIO, Optional

MAGIC = b"FOSIMG01"
HDR_SIZE = 4096
VERSION = 1
_FMT = "<8sI32s24s16sQ32sIQ"
_FMT_SIZE = struct.calcsize(_FMT)
assert _FMT_SIZE <= HDR_SIZE, "镜像头固定区必须小于 4096"

_CHUNK = 1024 * 1024   # 1 MiB 流式块


class ImageError(Exception):
    """镜像格式/校验错误。"""


@dataclass
class ImageHeader:
    device: str
    part: str
    kind: str
    size: int
    sha256: bytes
    seq: int
    created_ms: int

    def pack(self) -> bytes:
        head = struct.pack(
            _FMT, MAGIC, VERSION,
            self.device.encode("utf-8")[:32],
            self.part.encode("utf-8")[:24],
            self.kind.encode("utf-8")[:16],
            self.size, self.sha256, self.seq, self.created_ms)
        return head.ljust(HDR_SIZE, b"\0")


def load_header(path: str) -> tuple[ImageHeader, int]:
    """读镜像头，返回 (header, 负载起始偏移)。非法抛 ImageError。"""
    with open(path, "rb") as f:
        raw = f.read(HDR_SIZE)
    if len(raw) < HDR_SIZE:
        raise ImageError("文件太小，不是 .fosimg")
    (magic, ver, dev, part, kind, size, digest, seq,
     created) = struct.unpack(_FMT, raw[:_FMT_SIZE])
    if magic != MAGIC:
        raise ImageError(f"魔数不符: {magic!r}")
    if ver != VERSION:
        raise ImageError(f"不支持的镜像版本 {ver}")
    body = os.path.getsize(path) - HDR_SIZE
    if body < size:
        raise ImageError(f"负载截断: 声明 {size}，实际 {body}")
    return (ImageHeader(device=dev.rstrip(b"\0").decode(errors="replace"),
                        part=part.rstrip(b"\0").decode(errors="replace"),
                        kind=kind.rstrip(b"\0").decode(errors="replace"),
                        size=size, sha256=digest, seq=seq,
                        created_ms=created), HDR_SIZE)


def sha256_file(path: str, offset: int = 0, length: Optional[int] = None,
                progress=None) -> bytes:
    """对文件某段算 sha256。length=None 表示到文件末尾。"""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(offset)
        left = length if length is not None else os.fstat(f.fileno()).st_size - offset
        done = 0
        while left > 0:
            n = min(_CHUNK, left)
            buf = f.read(n)
            if not buf:
                break
            h.update(buf)
            left -= len(buf)
            done += len(buf)
            if progress:
                progress(done)
    return h.digest()


def write_stream(src: str, dst: BinaryIO, src_offset: int, size: int,
                 digest: Optional[bytes] = None,
                 progress=None) -> bytes:
    """把 src[src_offset:src_offset+size] 流式写入 dst。

    边写边算 sha256；给了 digest 就比对，不符抛 ImageError
    （调用方应在写入**之前**完成空间与权限检查）。
    返回实际写入内容的 sha256。
    """
    h = hashlib.sha256()
    written = 0
    with open(src, "rb") as f:
        f.seek(src_offset)
        while written < size:
            n = min(_CHUNK, size - written)
            buf = f.read(n)
            if not buf:
                raise ImageError(f"源文件提前结束：{written}/{size}")
            dst.write(buf)
            h.update(buf)
            written += len(buf)
            if progress:
                progress(written, size)
    got = h.digest()
    if digest is not None and got != digest:
        raise ImageError(
            f"写入校验失败：期望 {digest.hex()[:16]}… 实得 {got.hex()[:16]}…")
    return got


def make_placeholder(out_path: str, device: str = "pc",
                     part: str = "system_a", payload: bytes = b"") -> ImageHeader:
    """生成合法镜像头（工具链自测/社区做镜像打包时的脚手架）。"""
    hdr = ImageHeader(device=device, part=part, kind="raw",
                      size=len(payload),
                      sha256=hashlib.sha256(payload).digest(),
                      seq=1, created_ms=int(__import__("time").time() * 1000))
    with open(out_path, "wb") as f:
        f.write(hdr.pack())
        f.write(payload)
    return hdr
