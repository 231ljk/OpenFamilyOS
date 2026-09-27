# 转包引擎（apk / deb / rpm → .fos）

## 生态策略（开发文档 第六章）

| 对象 | 路径 |
| --- | --- |
| 主流应用 | 官方适配优先（原生 `.fos`） |
| 小厂应用 | **转包引擎**：APK / deb / rpm → `.fos`，运行在沙箱 |
| 独立开发者 | 低门槛上架：`python3 -m pkg build` 即可产出合规包 |

## 引擎做什么 / 不做什么

**做**：识别源格式 → 提取入口与负载 → 装箱进 `.fos` 布局 → 生成
`manifest.json` 与 `FOS-INF/converter.info`（记录来源与引擎版本）。

**不做**：不翻译指令、不重写代码、不模拟运行时。运行期能力由
`services/virt-compat`（ARM64 原生 / x86_64 翻译）与沙箱承担。
职责分离让两边都能独立演进。

## 各格式转换规则

### APK
- 入口来源：`lib/<abi>/*.so`、`assets/bin/<abi>/*`（原生负载）；
- 其余 `res/ assets/ AndroidManifest.xml classes.dex` 透传为
  `data/rootfs/`（沙箱只读根）；
- 包名/版本读取约定旁路：`assets/app.id`、`assets/app.version`
  （不解析二进制 AndroidManifest —— 需 aapt/axml 解码器，留给社区适配器）；
- 纯 Java/Kotlin 应用（无原生负载）会被明确拒绝并提示走官方 ART 适配。

### deb
- 解析 `ar` 容器 → `control.tar.gz`（Package/Version/Architecture）
  + `data.tar.gz`；
- 入口：`usr/bin/*` 第一个可执行；其余文件进 `data/rootfs/`；
- 架构映射：`amd64→x86_64`、`arm64→arm64`、`all→x86_64`（保守）；
- Epoch 版本（`1:2.3.4-1`）自动剥离。

### rpm
- 需要系统 `rpm2cpio`（rpm header 索引纯 Python 重写不划算）；
- 缺失时明确报错并给出下一步（安装 rpm2cpio，或先转 cpio）；
- 入口：`usr/bin/*`；其余进 rootfs；架构保守记 `x86_64`。

## 架构映射表

| 外部架构 | FamilyOS arch | 运行方式 |
| --- | --- | --- |
| `arm64-v8a` / `aarch64` | `arm64` | 原生 |
| `armeabi-v7a` / `armeabi` | `arm64` | 内核 32 位兼容 |
| `x86_64` / `amd64` | `x86_64` | 翻译后端（box64/fex/qemu） |
| `i386` / `i686` | `x86_64` | 同上（32 位子集） |

## 沙箱运行

转包包的 `manifest.entry.type = "bridge"`、`sandbox.mode = "strict"`：

```
bwrap --ro-bind /opt/fos/apps/<id>/<ver>/data/rootfs /   # 只读根
      --bind    ~/.fos/data/<id>      /data             # 私有可写
      --unshare-net                                       # 默认断网
      <entry>
```

（沙箱执行器属系统组件，本仓库文档化其行为；具体 launcher 实现欢迎社区 PR。）

## 失败即诚实

| 情况 | 引擎行为 |
| --- | --- |
| 纯 Java APK | 报错并说明转包面向原生负载 |
| deb 无 `usr/bin` | 报错：数据包装配请官方适配 |
| rpm 无 `rpm2cpio` | 报错并给出安装指引 |
| 源已是 `.fos` | 报错：无需转换 |

## 相关

- 格式规范：[docs/fos-package-format.md](../docs/fos-package-format.md)
- 运行层：[services/virt-compat](../services/virt-compat)
- 家长控制联动：[services/health](../services/health)（启动前 `can.launch`）
