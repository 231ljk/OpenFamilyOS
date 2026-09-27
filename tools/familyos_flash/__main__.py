# SPDX-License-Identifier: MulanPSL-2.0
"""允许 `python3 -m familyos_flash …` 直接调用 CLI。"""

import sys

from .cli import main

if __name__ == "__main__":
    sys.exit(main())
