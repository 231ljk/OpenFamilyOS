# kernel/ — FamilyOS 内核层（GPL-2.0）

本目录是 FamilyOS 开轮版对 Linux 内核做**二次开发**的载体。
开轮版不 fork 上游源码树，而是采用业界成熟且社区友好的三层结构：

```
上游 Linux LTS（基线，不改动）
    + patches/     少量上游补丁（可完全跳过，模块仍可用，仅能力降级）
    + *.ko         可加载模块（开轮版自研能力的主体，随时可移除）
```

这样设计的原因：

1. **可替换**：社区可以按需接入、替换或移除任何一个模块 —— 这正是「开轮版」的约定。
2. **可跟进**：基线内核随上游 LTS 滚动，不被版本锁死。
3. **共享框架**：闭源分支使用完全自研内核，但 `super_ring.h` / `health_usage.h`
   等接口契约与本目录一致，上层生态无需重写。

## 基线版本

| 项 | 值 |
| --- | --- |
| 对齐基线 | Linux 6.6 LTS（≥6.1 均可编译本目录模块） |
| 架构 | ARM64（主）、x86_64（次） |
| 许可证 | GPL-2.0-only（与上游一致） |

## 模块清单

| 模块 | 设备节点 / 接口 | 作用 |
| --- | --- | --- |
| [`super_ring/`](super_ring) | `/dev/fos_ring` | 超级环的设备注册表：账号下在线设备、投屏/互传/剪贴板/网络共享的会话登记，非本生态蓝牙外设也登记于此 |
| [`health_usage/`](health_usage) | `/dev/fos_health` | 健康使用的权威计数：按应用累计使用时长 + 禁用名单（名单由用户态守护进程执行拦截） |

## 构建

```bash
# 方式一：仓库顶层
make kernel

# 方式二：直接对内核构建树编译（out-of-tree）
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
```

加载/卸载：

```bash
sudo insmod super_ring.ko
sudo rmmod super_ring
```

## 配置（Kconfig）

`Kconfig` 是配置片段。如果你把开轮版编进自己的内核源码树，
在 `drivers/familyos/Kconfig` 里 `source "drivers/familyos/Kconfig"` 即可；
仅作为外部模块构建时无需理会。

## 补丁组织（patches/）

`patches/` 存放针对基线的少量补丁，规则见其 README。
所有补丁必须：独立可 cherry-pick、带 upstream 意向说明、不改变默认行为。

## 接口契约

- 模块与用户态之间只通过 ioctl / procfs / sysfs 通信，禁止符号导出依赖。
- 公共头文件（`*.h`）即 ABI 契约：只增字段不改语义，新增命令顺延编号。
- 参考实现保持「功能克制」：单文件、少依赖、全注释，欢迎社区替换为更完整的实现。
