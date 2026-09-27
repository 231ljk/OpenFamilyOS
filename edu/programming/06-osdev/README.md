# 06 · 系统开发（14+）★ 终章

读懂开轮版 IPC 骨架 · 写一个新系统服务并提 PR。

教材就是本仓库：

1. 从 `docs/ipc-protocol.md` 起，读 `services/common/`（约 700 行），
   理解「行协议 + poll 单线程」为什么够用；
2. 仿照 `services/health`，写你自己的第 8 个服务（建议：天气缓存、
   家庭留言板、番茄钟）；
3. 挂 systemd 单元、进 `services/Makefile`、写冒烟测试、提 PR。

从使用者变成创造者 —— 这就是「开轮」的闭环。
