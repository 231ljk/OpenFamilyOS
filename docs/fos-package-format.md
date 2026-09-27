# `.fos` 原生包格式规范（v1）

> FamilyOS 应用生态的原生分发单元。转包引擎（APK/deb/rpm → .fos）见
> [`../pkg/`](../pkg)。

## 1. 文件形态

`.fos` 就是一个 **ZIP（store 或 deflate）归档** + 强制首条目清单，
选择 ZIP 而非自造容器是为了：任何语言的库都能读、可流式校验、
转包引擎可直接嵌套既有解析器。**"格式自研"体现在清单 schema、
目录布局与签名约定上**，不重复造压缩容器。

```
hello.fos
├── FOS-INF/manifest.json     ← 必须第一个条目，未压缩（store）
├── FOS-INF/signature.json    ← 签名（可选但强烈建议；见 §4）
├── FOS-INF/converter.info    ← 仅转包包有（记录来源与引擎版本）
├── bin/…                     ← 可执行入口（按目标架构子目录）
│   bin/arm64/hello           ← ARM64 原生
│   bin/x86_64/hello          ← 经虚拟化兼容层翻译运行
├── lib/…                     ← 依赖库（可选）
├── res/…                     ← 图标/资源
└── data/…                    ← 只读资源数据
```

## 2. manifest.json（必填）

```json
{
  "format": 1,
  "id": "com.example.hello",
  "name": "Hello",
  "version_code": 3,
  "version_name": "1.2.0",
  "min_os": "0.1.0",
  "arch": ["arm64", "x86_64"],
  "entry": {
    "type": "binary",
    "path": "bin/{arch}/hello"
  },
  "permissions": ["health.usage.read"],
  "sandbox": {
    "mode": "strict",
    "fs_quota_mb": 128
  },
  "icon": "res/icon.png",
  "upgrade_from": ["apk:com.example.hello"]
}
```

| 字段 | 约束 |
| --- | --- |
| `id` | 反向域名，全局唯一，`[a-z0-9_.]`，≤ 128 |
| `arch` | `arm64` / `x86_64` 子集；x86_64 由虚拟化兼容层承接 |
| `entry.type` | `binary`（默认）；转包包可带 `bridge`（经兼容层） |
| `entry.path` | 支持 `{arch}` 占位；解析后必须真实存在于包内 |
| `sandbox.mode` | `strict`（默认，bwrap 隔离）/ `legacy`（转包调试用） |
| `permissions` | 系统能力枚举（`health.usage.read`、`ring.control`…） |
| `upgrade_from` | 转包引擎用：声明可从哪些外部来源升级为本包 |

## 3. 校验与安装

1. 包管理读取 `FOS-INF/manifest.json`，校验 schema；
2. 逐条目计算 sha256 写入 `.fos.lock`（安装后验证完整性）；
3. 解包到 `/opt/fos/apps/<id>/<version_code>/`，桌面挂图标；
4. 启动前经 `health.can.launch` 询问（家长禁用即拒绝）；
5. 运行于沙箱（strict 模式：只读 rootfs + 私有可写 data 目录）。

## 4. 签名（可选，v1 约定）

`FOS-INF/signature.json`：

```json
{
  "alg": "ed25519",
  "pubkey": "<base64>",
  "sig": "<base64>",
  "signed_at": "2026-09-27T12:00:00Z"
}
```

`sig` = 对除 `signature.json` 外全部条目 zip 内容的签名。
商店上架要求签名与 `id` 绑定；开源社区侧装包**不强制验签**
（开轮版哲学：不管控），但 UI 会醒目提示"未验证来源"。

## 5. 版本兼容

`format` 字段 = 大版本。v1 解析器遇到 `format >= 2` 拒绝安装并提示升级系统。
新增字段只加不改（解析器必须忽略未知字段）。

## 6. 工具链（本仓库）

| 操作 | 命令 |
| --- | --- |
| 打包 | `python3 -m pkg build examples/hello-app -o hello.fos` |
| 检视 | `python3 -m pkg inspect hello.fos` |
| 校验 | `python3 -m pkg verify hello.fos` |
| 转包 APK | `python3 -m pkg convert app.apk -o app.fos` |
| 转包 deb | `python3 -m pkg convert app.deb -o app.fos` |

源码在 [`pkg/`](../pkg)。
