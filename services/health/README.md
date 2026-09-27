# health — 健康使用设备（家长管理）

`fos-health-d`：跨设备健康使用功能的本机侧。覆盖电脑、平板、手机、
电视、手表。

## 分工

| 层 | 职责 |
| --- | --- |
| 内核 `kernel/health_usage` | 权威计时（/dev/fos_health 计数 + 禁用名单存放） |
| 本守护进程 | 名单持久化（health.json）、拦截执行、家长面板 IPC 门面 |
| 桌面/包管理器 | 启动应用前调 `can.launch` 询问；被禁应用撤回图标 |

内核模块不在时自动降级为纯本地记账（dev 环境可用）。

## IPC 命令

| 命令 | 说明 |
| --- | --- |
| `usage.list` | 用了哪些应用、使用了多少天 |
| `usage.add {pkg,uid,ms}` | 前台时长累计（单次上限 1h，防抽风） |
| `block.set {pkg,level}` | 禁用应用（level 0=可询问 1=硬禁用） |
| `block.clear {pkg}` | 解除禁用 |
| `can.launch {pkg,uid}` | 启动前询问 → `{allow,reason?}` |

## 数据

`$FOS_DATA_DIR/health.json`（默认 `/var/lib/fos/`）。家长密码校验、
跨设备聚合（多端 usage 经超级环归并）由账号服务与面板完成，
本服务只认本机记录。

## 试跑

```bash
make && ./build/fosd --self-test
FOS_DATA_DIR=/tmp/fosh ./build/fosd --sock /tmp/fos/health.sock &
echo '{"id":1,"cmd":"block.set","args":{"pkg":"com.example.game","level":1}}' \
  | nc -U /tmp/fos/health.sock
echo '{"id":2,"cmd":"can.launch","args":{"pkg":"com.example.game","uid":1000}}' \
  | nc -U /tmp/fos/health.sock
# → {"id":2,"ok":true,"result":{"allow":false,"reason":"家长已禁用（硬禁用）"}}
```
