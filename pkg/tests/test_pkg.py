# SPDX-License-Identifier: MIT
""".fos 打包 / 检视 / 校验 / 转包测试。

测试内现场合成 apk（zip）与 deb（ar+tar），不依赖任何外部样本文件。
"""

import io
import json
import os
import sys
import tarfile
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from pkg.builder import BuildError, build_package           # noqa: E402
from pkg.convert import ConvertError, convert_package, detect_source  # noqa: E402
from pkg.inspector import InspectError, inspect_package, verify_package  # noqa: E402
from pkg.manifest import ManifestError, validate_manifest   # noqa: E402

ELF = b"\x7fELF" + b"\x00" * 60   # 最小 ELF 魔数占位

GOOD = {
    "format": 1,
    "id": "com.example.hello",
    "name": "Hello",
    "version_code": 3,
    "version_name": "1.2.0",
    "min_os": "0.1.0",
    "arch": ["arm64", "x86_64"],
    "entry": {"type": "binary", "path": "bin/{arch}/hello"},
    "permissions": [],
    "sandbox": {"mode": "strict", "fs_quota_mb": 128},
    "icon": "res/icon.txt",
    "upgrade_from": [],
}


def make_app(dirpath: str, manifest: dict = None) -> str:
    os.makedirs(dirpath, exist_ok=True)
    man = json.loads(json.dumps(manifest if manifest is not None else GOOD))
    with open(os.path.join(dirpath, "manifest.json"), "w",
              encoding="utf-8") as f:
        json.dump(man, f, ensure_ascii=False)
    for arch in man["arch"]:
        d = os.path.join(dirpath, "bin", arch)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "hello"), "wb") as f:
            f.write(ELF)
    rd = os.path.join(dirpath, "res")
    os.makedirs(rd, exist_ok=True)
    with open(os.path.join(rd, "icon.txt"), "w") as f:
        f.write("icon")
    return dirpath


class TempCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.w = self.dir.name

    def p(self, *parts):
        return os.path.join(self.w, *parts)

    def tearDown(self):
        self.dir.cleanup()


# ─────────────────── manifest ───────────────────

class TestManifest(unittest.TestCase):
    def test_good(self):
        self.assertEqual(validate_manifest(GOOD), [])

    def test_bad_id(self):
        m = dict(GOOD, id="Hello World")
        self.assertTrue(any("id" in e for e in validate_manifest(m)))

    def test_bad_arch(self):
        m = dict(GOOD, arch=["mips"])
        self.assertTrue(any("架构" in e for e in validate_manifest(m)))

    def test_path_traversal(self):
        m = dict(GOOD, entry={"type": "binary", "path": "../evil"})
        self.assertTrue(any("越界" in e for e in validate_manifest(m)))

    def test_multiple_errors_at_once(self):
        m = dict(GOOD, id="X", version_code=0)
        self.assertGreaterEqual(len(validate_manifest(m)), 2)


# ─────────────────── build / inspect ───────────────────

class TestBuildInspect(TempCase):
    def test_roundtrip(self):
        src = make_app(self.p("app"))
        out = self.p("hello.fos")
        man = build_package(src, out)
        self.assertEqual(man.id, "com.example.hello")

        info = inspect_package(out)
        self.assertEqual(info["manifest"]["version_name"], "1.2.0")
        names = [e["name"] for e in info["entries"]]
        self.assertIn("bin/arm64/hello", names)
        self.assertIn("bin/x86_64/hello", names)

        ok, problems = verify_package(out)
        self.assertTrue(ok, problems)

    def test_manifest_is_first_entry_stored(self):
        out = self.p("a.fos")
        build_package(make_app(self.p("app")), out)
        with zipfile.ZipFile(out) as zf:
            first = zf.infolist()[0]
            self.assertEqual(first.filename, "FOS-INF/manifest.json")
            self.assertEqual(first.compress_type, zipfile.ZIP_STORED)

    def test_reproducible_manifest_bytes(self):
        src = make_app(self.p("app"))
        o1, o2 = self.p("1.fos"), self.p("2.fos")
        build_package(src, o1)
        build_package(src, o2)
        with zipfile.ZipFile(o1) as a, zipfile.ZipFile(o2) as b:
            self.assertEqual(a.read("FOS-INF/manifest.json"),
                             b.read("FOS-INF/manifest.json"))

    def test_missing_entry_rejected(self):
        src = self.p("app")
        make_app(src)
        os.remove(os.path.join(src, "bin", "x86_64", "hello"))
        with self.assertRaises(BuildError):
            build_package(src, self.p("x.fos"))

    def test_extra_root_file_rejected(self):
        src = make_app(self.p("app"))
        with open(os.path.join(src, "README.md"), "w") as f:
            f.write("nope")
        with self.assertRaises(BuildError):
            build_package(src, self.p("x.fos"))

    def test_unknown_dir_rejected(self):
        src = make_app(self.p("app"))
        os.makedirs(os.path.join(src, "share"))
        with open(os.path.join(src, "share", "f"), "w") as f:
            f.write("x")
        with self.assertRaises(BuildError):
            build_package(src, self.p("x.fos"))


# ─────────────────── verify（篡改检测）───────────────────

class TestVerify(TempCase):
    def _pack(self):
        out = self.p("a.fos")
        build_package(make_app(self.p("app")), out)
        return out

    def test_corrupt_detected(self):
        out = self._pack()
        # 直接篡改首清单（store 不压缩）数据区的一个字节
        with zipfile.ZipFile(out) as zf:
            zi = zf.infolist()[0]
            data_off = zi.header_offset + 30 + len(zi.filename.encode())
        with open(out, "r+b") as f:
            f.seek(data_off + 5)
            b = f.read(1)
            f.seek(-1, os.SEEK_CUR)
            f.write(bytes([b[0] ^ 0xFF]))
        ok, problems = verify_package(out)
        self.assertFalse(ok)
        self.assertTrue(problems)

    def test_not_zip(self):
        p = self.p("junk.fos")
        with open(p, "wb") as f:
            f.write(b"not a zip at all")
        ok, problems = verify_package(p)
        self.assertFalse(ok)
        self.assertTrue(any("ZIP" in x for x in problems))

    def test_inspect_missing_manifest(self):
        p = self.p("m.fos")
        with zipfile.ZipFile(p, "w") as zf:
            zf.writestr("bin/arm64/hello", ELF)
        with self.assertRaises(InspectError):
            inspect_package(p)


# ─────────────────── 转包：合成 apk/deb 样本 ───────────────────

def synth_apk(path: str, arch="arm64-v8a", extra_names=None):
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr(f"lib/{arch}/libhello.so", ELF)
        zf.writestr("AndroidManifest.xml", b"\x03\x00\x08\x00binary")
        zf.writestr("classes.dex", b"dex\n035\x00junk")
        zf.writestr("assets/app.id", b"com.sample.apkapp")
        zf.writestr("assets/app.version", b"9.9")
        for n in (extra_names or []):
            zf.writestr(n, b"x")
    return path


def _ar_hdr(name: str, size: int) -> bytes:
    h = (name.ljust(16).encode() + b"0           " + b"0     " +
         b"0     " + b"100644  " + str(size).ljust(10).encode() + b"`\n")
    assert len(h) == 60, len(h)
    return h


def _tar(members):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tf:
        for name, data, mode in members:
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mode = mode
            tf.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def synth_deb(path: str, pkg="widget", arch="amd64", bins=("widget",)):
    control = (f"Package: {pkg}\nVersion: 1:2.3.4-1\n"
               f"Architecture: {arch}\nMaintainer: x\nDescription: t\n"
               ).encode()
    ctl_tar = _tar([("./control", control, 0o644)])
    data_tar = _tar([
        (f"./usr/bin/{b}", ELF, 0o755) for b in bins
    ] + [("./usr/share/doc/widget/README", b"hello", 0o644)])
    with open(path, "wb") as f:
        f.write(b"!<arch>\n")
        for name, blob in (("debian-binary", b"2.0\n"),
                           ("control.tar.gz", ctl_tar),
                           ("data.tar.gz", data_tar)):
            f.write(_ar_hdr(name, len(blob)))
            f.write(blob)
            if len(blob) % 2:
                f.write(b"\n")
    return path


class TestConvert(TempCase):
    def test_detect_source(self):
        apk = synth_apk(self.p("a.apk"))
        deb = synth_deb(self.p("d.deb"))
        fos = self.p("x.fos")
        build_package(make_app(self.p("app")), fos)
        self.assertEqual(detect_source(apk), "apk")
        self.assertEqual(detect_source(deb), "deb")
        self.assertEqual(detect_source(fos), "fos")
        self.assertEqual(detect_source(self.p("nope.txt")), "unknown")

    def test_apk_to_fos(self):
        src = synth_apk(self.p("a.apk"), arch="arm64-v8a")
        out = self.p("a.fos")
        res = convert_package(src, out)
        self.assertEqual(res["id"], "com.sample.apkapp")
        self.assertEqual(res["archs"], ["arm64"])

        ok, problems = verify_package(out)
        self.assertTrue(ok, problems)
        info = inspect_package(out)
        self.assertEqual(info["manifest"]["entry"]["type"], "bridge")
        self.assertEqual(info["manifest"]["version_name"], "9.9")
        names = [e["name"] for e in info["entries"]]
        self.assertIn("FOS-INF/converter.info", names)
        self.assertIn("data/rootfs/assets/app.id", names)

    def test_apk_unknown_arch_rejected(self):
        src = synth_apk(self.p("u.apk"), arch="mips")
        with self.assertRaises(ConvertError):
            convert_package(src, self.p("u.fos"))

    def test_apk_native_only_java_rejected(self):
        src = self.p("j.apk")
        with zipfile.ZipFile(src, "w") as zf:
            zf.writestr("classes.dex", b"dex\n035\x00")
            zf.writestr("AndroidManifest.xml", b"\x03\x00")
        with self.assertRaises(ConvertError):
            convert_package(src, self.p("j.fos"))

    def test_deb_to_fos(self):
        src = synth_deb(self.p("d.deb"))
        out = self.p("d.fos")
        res = convert_package(src, out)
        # deb 裸包名 "widget" 无点号，规范化为 converted.widget
        self.assertEqual(res["id"], "converted.widget")
        self.assertEqual(res["archs"], ["x86_64"])     # amd64 映射
        ok, problems = verify_package(out)
        self.assertTrue(ok, problems)
        info = inspect_package(out)
        self.assertEqual(info["manifest"]["version_name"], "2.3.4-1")
        names = [e["name"] for e in info["entries"]]
        self.assertIn("bin/x86_64/widget", names)
        self.assertIn("data/rootfs/usr/share/doc/widget/README", names)

    def test_deb_no_entry_rejected(self):
        # 只有文档没有 usr/bin：应拒绝
        path = self.p("e.deb")
        control = b"Package: docsonly\nVersion: 1.0\nArchitecture: amd64\n"
        with open(path, "wb") as f:
            f.write(b"!<arch>\n")
            for name, blob in (("debian-binary", b"2.0\n"),
                               ("control.tar.gz",
                                _tar([("./control", control, 0o644)])),
                               ("data.tar.gz",
                                _tar([("./usr/share/x", b"y", 0o644)]))):
                f.write(_ar_hdr(name, len(blob)))
                f.write(blob)
                if len(blob) % 2:
                    f.write(b"\n")
        with self.assertRaises(ConvertError):
            convert_package(path, self.p("e.fos"))

    def test_convert_already_fos(self):
        fos = self.p("x.fos")
        build_package(make_app(self.p("app")), fos)
        with self.assertRaises(ConvertError):
            convert_package(fos, self.p("y.fos"))

    def test_override_manifest_id(self):
        src = synth_apk(self.p("a.apk"))
        out = self.p("a2.fos")
        res = convert_package(src, out, manifest_override={"id": "com.re.named"})
        self.assertEqual(res["id"], "com.re.named")


# ─────────────────── CLI ───────────────────

class TestCli(TempCase):
    def test_cli_build_verify_inspect(self):
        from pkg.__main__ import main

        src = make_app(self.p("app"))
        out = self.p("cli.fos")
        self.assertEqual(main(["build", src, "-o", out]), 0)
        self.assertEqual(main(["verify", out]), 0)
        self.assertEqual(main(["inspect", out]), 0)
        self.assertEqual(main(["convert", out, "-o", self.p("z.fos")]), 1)

    def test_cli_build_bad_manifest(self):
        from pkg.__main__ import main

        bad = dict(GOOD, id="Not.An Id")
        src = make_app(self.p("app2"), bad)
        self.assertEqual(main(["build", src, "-o", self.p("b.fos")]), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
