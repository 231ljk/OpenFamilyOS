# SPDX-License-Identifier: MIT
"""`python3 -m pkg` —— .fos 包工具链 CLI。

    python3 -m pkg build   <src_dir>  -o hello.fos
    python3 -m pkg inspect hello.fos
    python3 -m pkg verify  hello.fos
    python3 -m pkg convert app.apk   -o app.fos
"""

from __future__ import annotations

import argparse
import json
import sys

from .builder import BuildError, build_package
from .convert import ConvertError, convert_package, detect_source
from .inspector import InspectError, inspect_package, verify_package
from .manifest import ManifestError


def _cmd_build(a: argparse.Namespace) -> int:
    try:
        man = build_package(a.src, a.out)
    except (BuildError, ManifestError) as e:
        errs = getattr(e, "errors", None)
        if errs:
            for m in errs:
                print(f"错误: {m}", file=sys.stderr)
        else:
            print(f"错误: {e}", file=sys.stderr)
        return 1
    print(f"已打包 {man.id} v{man.version_name} → {a.out}")
    return 0


def _cmd_inspect(a: argparse.Namespace) -> int:
    try:
        info = inspect_package(a.pkg)
    except InspectError as e:
        print(f"错误: {e}", file=sys.stderr)
        return 1
    print(json.dumps(info, ensure_ascii=False, indent=2))
    return 0


def _cmd_verify(a: argparse.Namespace) -> int:
    ok, problems = verify_package(a.pkg)
    for p in problems:
        print(f"✗ {p}", file=sys.stderr)
    if ok:
        print("✓ 校验通过")
        return 0
    return 1


def _cmd_convert(a: argparse.Namespace) -> int:
    try:
        res = convert_package(a.src, a.out)
    except (ConvertError, ManifestError) as e:
        print(f"错误: {e}", file=sys.stderr)
        return 1
    print(f"转换成功: {a.src} [{detect_source(a.src)}] → {res['out']}")
    print(f"  id     : {res['id']}")
    print(f"  entry  : {res['entry']}")
    print(f"  archs  : {res['archs']}")
    print("运行环境：strict 沙箱 + 虚拟化兼容层（x86_64 由翻译后端承接）")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python3 -m pkg",
                                 description="FamilyOS .fos 包工具链")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("build", help="源目录 → .fos")
    p.add_argument("src")
    p.add_argument("-o", "--out", required=True)
    p.set_defaults(fn=_cmd_build)

    p = sub.add_parser("inspect", help="查看 .fos 内容")
    p.add_argument("pkg")
    p.set_defaults(fn=_cmd_inspect)

    p = sub.add_parser("verify", help="校验 .fos 完整性")
    p.add_argument("pkg")
    p.set_defaults(fn=_cmd_verify)

    p = sub.add_parser("convert", help="apk/deb/rpm → .fos")
    p.add_argument("src")
    p.add_argument("-o", "--out", required=True)
    p.set_defaults(fn=_cmd_convert)

    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
