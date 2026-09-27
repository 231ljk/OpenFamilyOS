# SPDX-License-Identifier: MulanPSL-2.0
"""FamilyOS .fos 包工具链：打包 / 检视 / 校验 / 转包。

零第三方依赖（zipfile/tarfile/hashlib/json 走标准库）。
格式规范见 docs/fos-package-format.md。
"""

from .manifest import (Manifest, ManifestError, VALID_ARCH, VALID_ENTRY_TYPES,
                      validate_manifest)
from .builder import build_package
from .inspector import inspect_package, verify_package
from .convert import convert_package, detect_source

__all__ = [
    "Manifest", "ManifestError", "validate_manifest",
    "VALID_ARCH", "VALID_ENTRY_TYPES",
    "build_package", "inspect_package", "verify_package",
    "convert_package", "detect_source",
]
