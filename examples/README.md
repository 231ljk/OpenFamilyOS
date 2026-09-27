# examples/hello-app — 第一个 .fos 包

README 快速开始里的 `python3 -m pkg build examples/hello-app` 用的就是这个目录。

## 目录

```
hello-app/
├── manifest.json      # 包清单（schema 见 docs/fos-package-format.md）
├── bin/arm64/hello    # 入口脚本（sh 演示；真应用换 ELF）
├── bin/x86_64/hello
└── res/icon.txt       # 图标占位
```

## 打包 → 检视 → 校验

```bash
python3 -m pkg build examples/hello-app -o out/hello.fos
python3 -m pkg inspect out/hello.fos
python3 -m pkg verify  out/hello.fos
```

## 想要真二进制？

仓库同时提供 `hello.c`（同一目录旁置亦可），在 Linux 上交叉编译两架构：

```c
/* hello.c —— 交叉编译：
 *   aarch64-linux-gnu-gcc -static hello.c -o hello-app/bin/arm64/hello
 *   gcc -static hello.c -o hello-app/bin/x86_64/hello
 *   （无交叉链时用本机 gcc 只打 x86_64，manifest 里删掉 arm64 即可）
 */
#include <stdio.h>
int main(void) { printf("Hello from .fos native binary!\n"); return 0; }
```

## 下一步

把 `out/hello.fos` 装进构建出的系统镜像（`tools/build-image.sh`），
体验：桌面图标 → 沙箱启动 → 健康使用时长记账 → 家长禁用 →
`can.launch` 拦截。一条链走通，就是开轮版的闭环。
