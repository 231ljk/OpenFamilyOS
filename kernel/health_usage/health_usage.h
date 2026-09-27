/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * health_usage.h — 健康使用设备（Health Usage）内核/用户态接口契约
 *
 * 系统提供跨设备的健康使用功能，帮助家长管理设备：
 *   - 实时查看应用使用情况：用了哪些应用、各用了多少
 *   - 家长可随时禁用指定应用
 *
 * 分工（功能克制原则）：
 *   内核（本模块）＝ 权威计时器 + 禁用名单的存放地；
 *   拦截动作由用户态 services/health 守护进程执行（它读取名单后
 *   决定放行/阻止应用，并同步各端图标）。名单放在内核是为了跨用户
 *   的统一视图；掉电持久化由守护进程负责存盘，参考实现不做持久化。
 *
 * 计数维度：按 (uid, app_key) 累计前台毫秒数。app_key 是 .fos 包名
 * 的内核侧哈希（守护进程上报包名时登记映射），避免字符串进热路径。
 *
 * 本头文件是 ABI 契约：只增字段不改语义，命令编号只顺延不复用。
 */
#ifndef _UAPI_FOS_HEALTH_H
#define _UAPI_FOS_HEALTH_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define FOS_HEALTH_APP_MAX   128   /* 被记录的应用数上限（LRU 逐出最旧） */
#define FOS_HEALTH_NAME_MAX  64    /* 应用 id 字符串（包名）最大长度 */
#define FOS_HEALTH_BLOCK_MAX 64    /* 禁用名单容量 */

/* 查询/登记条目：id 为包名字符串，内核哈希成 app_key */
struct fos_health_app {
	__u32 app_key;                 /* 内核分配的数值 id（0 非法） */
	__u32 uid;                     /* 属主用户 */
	union {
		__u64 fg_ms;           /* GET/REGISTER：累计前台毫秒 */
		__u64 fg_ms_delta;     /* ACCOUNT：本次新增毫秒（上限 1h） */
	};
	__u64 last_active_ms;          /* 最近一次活跃（单调时钟） */
	__u64 day_ms[7];               /* 滚动 7 日每日毫秒，索引=周几-1 */
	char  id[FOS_HEALTH_NAME_MAX]; /* .fos 包名，如 com.example.notes */
};

/* 禁用名单条目 */
struct fos_health_block {
	__u32 app_key;
	__u32 uid;                     /* 0 = 对所有受限档用户生效 */
	__u8  level;                   /* 0=可询问解锁 1=硬禁用（家长密码） */
	__u8  reserved;
	char  id[FOS_HEALTH_NAME_MAX];
};

#define FOS_HEALTH_IOC_MAGIC 'H'

/* 登记应用名 → app_key（幂等：已存在则回填既有 key） */
#define FOS_HEALTH_IOC_REGISTER  _IOWR(FOS_HEALTH_IOC_MAGIC, 1, struct fos_health_app)
/* 累计一次前台时长：按 app_key(+uid) 累加 fg_ms / day_ms */
#define FOS_HEALTH_IOC_ACCOUNT   _IOW(FOS_HEALTH_IOC_MAGIC, 2, struct fos_health_app)
/* 查询单个应用统计 */
#define FOS_HEALTH_IOC_GET_APP   _IOWR(FOS_HEALTH_IOC_MAGIC, 3, struct fos_health_app)
/* 列出第 idx 条记录（idx 由 app_key=0 开始，内核回填下一条；返回 -ENOENT 结束） */
#define FOS_HEALTH_IOC_ENUM_APP  _IOWR(FOS_HEALTH_IOC_MAGIC, 4, struct fos_health_app)
/* 把应用加入禁用名单（幂等） */
#define FOS_HEALTH_IOC_BLOCK     _IOW(FOS_HEALTH_IOC_MAGIC, 5, struct fos_health_block)
/* 从禁用名单移除 */
#define FOS_HEALTH_IOC_UNBLOCK   _IOW(FOS_HEALTH_IOC_MAGIC, 6, struct fos_health_block)
/* 查询是否被禁用：入参 app_key+uid，被禁返回 0，未禁返回 -ENOENT */
#define FOS_HEALTH_IOC_IS_BLOCKED _IOW(FOS_HEALTH_IOC_MAGIC, 7, struct fos_health_block)
/* 清空全部统计数据（家长复位/换机迁出时用） */
#define FOS_HEALTH_IOC_RESET     _IO(FOS_HEALTH_IOC_MAGIC, 8)

#endif /* _UAPI_FOS_HEALTH_H */
