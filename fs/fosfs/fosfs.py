# SPDX-License-Identifier: MIT
"""fosfs.py — FamilyOS 自研存储格式的 Python 参考实现。

与 SPEC.md / fosfs_spec.h 逐字节一致。零第三方依赖（zlib/sha256 走标准库）。

快速上手::

    from fosfs import FosVolume

    vol = FosVolume.create("data.fosvol")
    vol.put("notes.txt", b"hello fosfs")
    vol.snapshot("v1")
    vol.put("notes.txt", b"changed")
    vol.rollback("v1")
    assert vol.get("notes.txt") == b"hello fosfs"
    vol.verify()          # 全盘校验；坏块抛 CorruptError

能力对照开发文档「系统内部存储：自研格式，提供加密、压缩、快照、
元数据扩展」：

    压缩      块级 zlib（comp 字段记录算法；压不动自动回退 raw）
    快照      不可变根块 + 清单写时复制（rollback 即根→新清单）
    元数据扩展 TLV 区（本实现演示 tag=0x0001 缓存 TTL）
    加密      布局已定义（enc 字段/密钥槽），本参考实现不启用，
              读侧遇加密块明确报 NotSupported（SPEC §3）

内容寻址：数据块以 sha256 为身份；清单块按序追加、读取取最后一个；
删除只追加墓碑；compact 重写活块回收孤儿。
"""

from __future__ import annotations

import hashlib
import json
import os
import struct
import time
import zlib
from typing import Dict, Optional

SB_MAGIC = b"FOSFS001"
SB_SIZE = 512
SB_VERSION = 1

CH_MAGIC = b"FOSCHK01"
CH_HDR_SIZE = 64

T_DATA, T_MANIFEST, T_SNAPROOT, T_TOMB = 1, 2, 3, 4
COMP_RAW, COMP_ZLIB = 0, 1
ENC_NONE, ENC_SLOT1 = 0, 1

EXT_TTL = 0x0001

# superblock 定长区（64B，其余 reserved 填充到 512）：
# magic8 ver4 flags4 created8 uuid16 hash4 comp4 count8 next8
_SB_FMT = "<8sIIQ16sIIQQ"
# chunk header（64B）：
# magic8 type1 comp1 enc1 rsv1 name_len2 payload_len4 ext_len2 seq4 raw_len8 sha32
_CH_FMT = "<8s4BHIHIQ32s"
assert struct.calcsize(_SB_FMT) == 64
assert struct.calcsize(_CH_FMT) == 64


class FosFSError(Exception):
    """fosfs 通用错误。"""


class CorruptError(FosFSError):
    """磁盘内容校验失败（魔数缺失/长度越界/哈希不符）。"""


class NotSupported(FosFSError):
    """遇到本参考实现未启用的特性（如加密块）。"""


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class FosVolume:
    """一个 fosfs 卷（单文件承载）。块流只追加；读侧重放得到当前视图。"""

    def __init__(self, path: str):
        self.path = path
        self._load_super()
        self._replay()

    # ── 卷生命周期 ─────────────────────────────────────────

    @classmethod
    def create(cls, path: str) -> "FosVolume":
        if os.path.exists(path):
            raise FosFSError(f"卷已存在: {path}")
        with open(path, "wb") as f:
            f.write(struct.pack(
                _SB_FMT, SB_MAGIC, SB_VERSION, 0,
                int(time.time() * 1000), os.urandom(16),
                0, COMP_ZLIB, 0, SB_SIZE))
            f.write(b"\0" * (SB_SIZE - 64))
        return cls(path)

    def _load_super(self) -> None:
        try:
            with open(self.path, "rb") as f:
                head = f.read(SB_SIZE)
        except FileNotFoundError:
            raise CorruptError(f"卷不存在: {self.path}") from None
        if len(head) < SB_SIZE:
            raise CorruptError("superblock 缺失（文件太短）")
        (magic, ver, self.flags, self.created_ms, self.uuid,
         self.hash_algo, self.comp_algo, _cnt, next_off) = struct.unpack(
            _SB_FMT, head[:64])
        if magic != SB_MAGIC:
            raise CorruptError(f"魔数不符: {magic!r}")
        if ver != SB_VERSION:
            raise CorruptError(f"不支持版本 {ver}（认 {SB_VERSION}）")
        self.next_off = max(next_off, SB_SIZE)

    def _write_super(self) -> None:
        with open(self.path, "r+b") as f:
            f.seek(0)
            f.write(struct.pack(
                _SB_FMT, SB_MAGIC, SB_VERSION, self.flags,
                self.created_ms, self.uuid, self.hash_algo,
                self.comp_algo, self._chunk_count, self.next_off))

    # ── 块读写 ─────────────────────────────────────────────

    def _append_chunk(self, ctype: int, name: str, payload: bytes,
                      ext: bytes = b"", comp: int = COMP_ZLIB,
                      enc: int = ENC_NONE) -> str:
        if enc != ENC_NONE:
            raise NotSupported("加密块写入未启用（SPEC §3）")
        blob = zlib.compress(payload, 6) if comp == COMP_ZLIB else payload
        if comp == COMP_ZLIB and len(blob) >= len(payload):
            comp, blob = COMP_RAW, payload   # 小对象压不动：如实记 raw
        digest = _sha(payload)

        hdr = struct.pack(_CH_FMT, CH_MAGIC, ctype, comp, enc, 0,
                          len(name.encode()), len(blob), len(ext),
                          self._seq, len(payload),
                          bytes.fromhex(digest))
        assert len(hdr) == CH_HDR_SIZE
        with open(self.path, "ab") as f:
            f.write(hdr)
            f.write(name.encode())
            f.write(ext)
            f.write(blob)
            self.next_off = f.tell()
        self._seq += 1
        self._chunk_count += 1
        return digest

    def _iter_chunks(self):
        """顺序扫描全部块；每块校验 sha256（解压后），坏块抛 CorruptError。"""
        with open(self.path, "rb") as f:
            f.seek(SB_SIZE)
            while True:
                at = f.tell()
                hdr = f.read(CH_HDR_SIZE)
                if not hdr:
                    return
                if len(hdr) < CH_HDR_SIZE:
                    raise CorruptError(f"块头截断 @off={at}")
                (magic, ctype, comp, enc, _rsv, name_len, plen,
                 ext_len, seq, raw_len, digest) = struct.unpack(
                    _CH_FMT, hdr)
                if magic != CH_MAGIC:
                    raise CorruptError(f"块魔数不符 @off={at}")
                if enc != ENC_NONE:
                    raise NotSupported(f"加密块 @off={at}：参考实现不解析")
                name = f.read(name_len).decode("utf-8", "replace")
                ext = f.read(ext_len)
                blob = f.read(plen)
                if len(blob) != plen:
                    raise CorruptError(f"payload 截断 @off={at}")
                try:
                    raw = (zlib.decompress(blob) if comp == COMP_ZLIB
                           else blob)
                except zlib.error as e:
                    raise CorruptError(f"{name}: 解压失败（{e}）") from e
                if len(raw) != raw_len:
                    raise CorruptError(f"{name}: 解压长度不符")
                if bytes.fromhex(_sha(raw)) != digest:
                    raise CorruptError(f"{name}: 内容哈希不符（静默损坏？）")
                yield {"type": ctype, "comp": comp, "seq": seq,
                       "name": name, "ext": ext, "payload": raw}

    # ── 视图重放 ───────────────────────────────────────────

    def _replay(self) -> None:
        self._files: Dict[str, bytes] = {}
        self._snapshots: Dict[str, Dict] = {}
        self._seq = 0
        self._chunk_count = 0
        last_manifest: Optional[bytes] = None
        data_index: Dict[str, bytes] = {}

        for ch in self._iter_chunks():
            self._seq = max(self._seq, ch["seq"] + 1)
            self._chunk_count += 1
            if ch["type"] == T_DATA:
                data_index[_sha(ch["payload"])] = ch["payload"]
            elif ch["type"] == T_MANIFEST:
                last_manifest = ch["payload"]
            elif ch["type"] == T_SNAPROOT:
                self._snapshots[ch["name"]] = json.loads(
                    ch["payload"].decode())
            # T_TOMB 仅作 compact 提示；视图以清单为准

        if last_manifest:
            for name, digest in json.loads(
                    last_manifest.decode()).get("files", {}).items():
                blob = data_index.get(digest)
                if blob is None:
                    raise CorruptError(f"清单引用缺失内容 {digest[:12]}")
                self._files[name] = blob

    def _current_manifest(self) -> Dict:
        return {"files": {n: _sha(b) for n, b in self._files.items()},
                "seq": self._seq}

    def _flush_manifest(self) -> None:
        self._append_chunk(T_MANIFEST, "manifest",
                           json.dumps(self._current_manifest()).encode())

    # ── 对外 API ───────────────────────────────────────────

    def put(self, name: str, data: bytes, ttl_ms: int = 0) -> str:
        """写入对象（覆盖语义）。ttl_ms>0 时写扩展区标签（缓存用法）。"""
        if not name or "/" in name or "\\" in name:
            raise FosFSError(f"非法对象名: {name!r}")
        ext = struct.pack("<HHQ", EXT_TTL, 8, ttl_ms) if ttl_ms else b""
        digest = self._append_chunk(T_DATA, name, data, ext=ext)
        self._files[name] = data
        self._flush_manifest()
        self._write_super()
        return digest

    def get(self, name: str) -> bytes:
        if name not in self._files:
            raise FosFSError(f"对象不存在: {name}")
        return self._files[name]

    def exists(self, name: str) -> bool:
        return name in self._files

    def delete(self, name: str) -> None:
        if name not in self._files:
            raise FosFSError(f"对象不存在: {name}")
        self._append_chunk(T_TOMB, name, b"")
        del self._files[name]
        self._flush_manifest()
        self._write_super()

    def ls(self) -> Dict[str, int]:
        return {n: len(b) for n, b in sorted(self._files.items())}

    def ttl_of(self, name: str) -> int:
        """读回 put 时写的 TTL（演示 TLV 扩展区；无则 0）。"""
        digest = _sha(self.get(name))
        for ch in self._iter_chunks():
            if ch["type"] != T_DATA:
                continue
            if _sha(ch["payload"]) == digest and len(ch["ext"]) >= 12:
                tag, ln, val = struct.unpack("<HHQ", ch["ext"][:12])
                if tag == EXT_TTL and ln == 8:
                    return int(val)
        return 0

    # ── 快照 ───────────────────────────────────────────────

    def snapshot(self, snap_name: str) -> None:
        """命名快照：把当前清单固化为不可变根块。"""
        if not snap_name or snap_name in self._snapshots:
            raise FosFSError(f"快照名非法或已存在: {snap_name!r}")
        man = self._current_manifest()
        self._append_chunk(T_SNAPROOT, snap_name,
                           json.dumps(man).encode())
        self._snapshots[snap_name] = man
        self._write_super()

    def snapshots(self):
        return sorted(self._snapshots)

    def rollback(self, snap_name: str) -> None:
        """回滚到快照：根块清单改写为新清单（旧块保留至 compact）。"""
        if snap_name not in self._snapshots:
            raise FosFSError(f"无此快照: {snap_name}")
        idx = self._snapshots[snap_name].get("files", {})
        data_index: Dict[str, bytes] = {}
        for ch in self._iter_chunks():
            if ch["type"] == T_DATA:
                data_index[_sha(ch["payload"])] = ch["payload"]
        files = {}
        for name, digest in idx.items():
            blob = data_index.get(digest)
            if blob is None:
                raise CorruptError(f"快照 {snap_name} 引用缺失 {digest[:12]}")
            files[name] = blob
        self._files = files
        self._flush_manifest()
        self._write_super()

    # ── 维护 ───────────────────────────────────────────────

    def verify(self) -> int:
        """全盘扫描：逐块校验哈希 + 清单引用闭合。返回块数。"""
        n = 0
        data_index: Dict[str, bytes] = {}
        for ch in self._iter_chunks():
            n += 1
            if ch["type"] == T_DATA:
                data_index[_sha(ch["payload"])] = ch["payload"]
        for digest in self._current_manifest()["files"].values():
            if digest not in data_index:
                raise CorruptError(f"清单引用悬空 {digest[:12]}")
        return n

    def compact(self) -> int:
        """物理整理：只重写活块（当前清单 + 全部快照根），回收孤儿。"""
        before = os.path.getsize(self.path)
        tmp = self.path + ".compact"
        if os.path.exists(tmp):
            os.remove(tmp)
        saved = FosVolume.create(tmp)
        saved._files = dict(self._files)        # 视图先行：清单据此生成
        saved._snapshots = dict(self._snapshots)
        for name, blob in self._files.items():
            saved._append_chunk(T_DATA, name, blob)
        for sname, man in self._snapshots.items():
            saved._append_chunk(T_SNAPROOT, sname, json.dumps(man).encode())
        saved._flush_manifest()
        saved._write_super()
        os.replace(tmp, self.path)
        self._load_super()
        self._replay()
        return before - os.path.getsize(self.path)
