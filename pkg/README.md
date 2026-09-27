# pkg — .fos 原生包与转包引擎

应用生态的工具链。格式规范见 [docs/fos-package-format.md](../docs/fos-package-format.md)，
转包策略见 [engine.md](engine.md)。

## 快速上手

```bash
# 1. 打一个示例包
python3 -m pkg build examples/hello-app -o hello.fos
python3 -m pkg inspect hello.fos
python3 -m pkg verify  hello.fos

# 2. 转包（APK → .fos；deb 同理；rpm 需系统 rpm2cpio）
python3 -m pkg convert yourapp.apk -o yourapp.fos

# 3. 测试
python3 -m unittest discover -s pkg/tests
```

## 模块结构

| 文件 | 职责 |
| --- | --- |
| `manifest.py` | 清单 schema 校验（一次报全所有错误）、读取 |
| `builder.py` | 源目录 → .fos（首条目固定、入口逐架构验证、可复现清单） |
| `inspector.py` | 检视（inspect）与完整性校验（verify：路径安全/CRC/入口存在） |
| `convert.py` | 转包引擎：APK / deb / rpm → .fos（沙箱 rootfs 装箱） |
| `__main__.py` | CLI：build / inspect / verify / convert |

## 设计要点

- **零第三方依赖**：zipfile/tarfile/io/hashlib 全部标准库 —— 任何 Linux
  装了 Python 3.10+ 就能打包分发，符合「独立开发者低门槛上架」；
- **转包不翻译指令**：只做清单化+装箱，运行时由 `virt-compat`（翻译层）
  与沙箱承接 —— 职责分离，引擎可独立演进；
- **诚实报错**：纯 Java APK（无原生负载）、无 `usr/bin` 的 deb 数据包装配、
  缺 `rpm2cpio` 的环境 —— 都给出明确原因与下一步建议，不假装成功。
