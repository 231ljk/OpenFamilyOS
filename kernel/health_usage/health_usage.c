// SPDX-License-Identifier: GPL-2.0
/*
 * health_usage.c — FamilyOS 健康使用：应用使用量计数与禁用名单（参考实现）
 *
 * 职责边界（功能克制原则）：
 *   本模块只做两件事：
 *     1. 按 (uid, app_key) 权威累计应用前台使用时长（含滚动 7 日）；
 *     2. 存放家长设置的「禁用应用名单」，供用户态守护进程查询拦截。
 *   发现前台应用、弹窗提醒、家长密码校验、名单持久化，全部在
 *   services/health 用户态守护进程完成。
 *
 * 接口：
 *   /dev/fos_health        ioctl（契约见 health_usage.h）
 *   /proc/fos_health       人类可读快照（家长面板 CLI 直接 cat）
 *
 * 存储结构：固定容量数组（APP_MAX / BLOCK_MAX），线性查找。
 * 应用数量级在百以内，线性表足够且便于社区阅读改造；
 * 满时按 last_active 最旧逐出（统计丢失可接受，禁用名单不逐出）。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/jhash.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/timekeeping.h>

#include "health_usage.h"

#define DRIVER_NAME "fos_health"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("FamilyOS Lab");
MODULE_DESCRIPTION("FamilyOS health usage counters and app block list");
MODULE_VERSION("0.1");

/* ───────────────────────── 内部状态 ───────────────────────── */

struct app_slot {
	bool used;
	u32 app_key;
	u32 uid;
	u64 fg_ms;
	u64 last_active_ms;
	u64 day_ms[7];     /* 按自然日累计；跨天时整体左移滚动 */
	u32 day_anchor;    /* 锚定日（自 epoch 的天数），用于跨天判定 */
	char id[FOS_HEALTH_NAME_MAX];
};

struct block_slot {
	bool used;
	u32 app_key;
	u32 uid;           /* 0=对所有受限用户 */
	u8 level;
	char id[FOS_HEALTH_NAME_MAX];
};

static DEFINE_MUTEX(health_lock);
static struct app_slot apps[FOS_HEALTH_APP_MAX];
static struct block_slot blocks[FOS_HEALTH_BLOCK_MAX];

static dev_t dev_no;
static struct class *health_class;
static struct cdev health_cdev;
static struct device *health_device;

/* ───────────────────────── 工具函数 ───────────────────────── */

static u32 app_hash(const char *id)
{
	u32 h = jhash(id, strnlen(id, FOS_HEALTH_NAME_MAX - 1) + 1, 0xF05);

	return h ? h : 1;      /* 0 保留为非法值 */
}

/* 当前"天序号"：REALTIME 秒 / 86400，用于 7 日滚动 */
static u32 today_epoch_day(void)
{
	return (u32)ktime_get_real_seconds() / 86400;
}

/* 调用方持锁：找应用槽，未找到返回 NULL */
static struct app_slot *app_find(u32 app_key, u32 uid)
{
	int i;

	for (i = 0; i < FOS_HEALTH_APP_MAX; i++)
		if (apps[i].used && apps[i].app_key == app_key && apps[i].uid == uid)
			return &apps[i];
	return NULL;
}

/* 调用方持锁：确保 app(+uid) 有槽位；满则逐出最久未活跃者 */
static struct app_slot *app_ensure(u32 app_key, u32 uid, const char *id)
{
	struct app_slot *sl = app_find(app_key, uid);
	int i;
	u64 oldest = U64_MAX;

	if (sl) {
		if (id && id[0])
			strscpy(sl->id, id, FOS_HEALTH_NAME_MAX);
		return sl;
	}
	for (i = 0; i < FOS_HEALTH_APP_MAX; i++) {
		if (!apps[i].used) {
			sl = &apps[i];
			goto init;
		}
		if (apps[i].last_active_ms < oldest) {
			oldest = apps[i].last_active_ms;
			sl = &apps[i];
		}
	}
	/* 逐出：保留禁用名单（名单单独存放，不受统计逐出影响） */
init:
	memset(sl, 0, sizeof(*sl));
	sl->used = true;
	sl->app_key = app_key;
	sl->uid = uid;
	if (id && id[0])
		strscpy(sl->id, id, FOS_HEALTH_NAME_MAX);
	sl->day_anchor = today_epoch_day();
	return sl;
}

/* 调用方持锁：把计时槽滚动到"今天"，跨天则左移 7 日窗口 */
static void app_roll_to_today(struct app_slot *sl)
{
	u32 day = today_epoch_day(), gap = day - sl->day_anchor;

	if (!gap)
		return;
	if (gap >= 7) {
		memset(sl->day_ms, 0, sizeof(sl->day_ms));
	} else {
		int i;

		for (i = 6; i >= 0; i--)
			sl->day_ms[i] = (i >= (int)gap) ? sl->day_ms[i - gap] : 0;
	}
	sl->day_anchor = day;
}

/* 调用方持锁：查禁用名单 */
static struct block_slot *block_find(u32 app_key, u32 uid)
{
	int i;

	for (i = 0; i < FOS_HEALTH_BLOCK_MAX; i++)
		if (blocks[i].used && blocks[i].app_key == app_key &&
		    (blocks[i].uid == uid || blocks[i].uid == 0))
			return &blocks[i];
	return NULL;
}

/* ──────────────────────── ioctl 分发 ──────────────────────── */

static long health_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	mutex_lock(&health_lock);

	switch (cmd) {
	case FOS_HEALTH_IOC_REGISTER: {
		struct fos_health_app req;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
			ret = -EFAULT;
			break;
		}
		if (!req.id[0]) {
			ret = -EINVAL;
			break;
		}
		req.app_key = app_hash(req.id);
		if (!app_ensure(req.app_key, req.uid, req.id)) {
			ret = -ENOSPC;
			break;
		}
		if (copy_to_user((void __user *)arg, &req, sizeof(req)))
			ret = -EFAULT;
		break;
	}
	case FOS_HEALTH_IOC_ACCOUNT: {
		struct fos_health_app req;
		struct app_slot *sl;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
			ret = -EFAULT;
			break;
		}
		if (!req.app_key || req.fg_ms_delta > 60 * 60 * 1000ULL) {
			ret = -EINVAL;  /* 单次增量上限 1 小时，防用户态抽风 */
			break;
		}
		sl = app_ensure(req.app_key, req.uid, req.id);
		if (!sl) {
			ret = -ENOSPC;
			break;
		}
		app_roll_to_today(sl);
		sl->fg_ms += req.fg_ms_delta;
		sl->day_ms[6] += req.fg_ms_delta;
		sl->last_active_ms = ktime_to_ms(ktime_get());
		break;
	}
	case FOS_HEALTH_IOC_GET_APP: {
		struct fos_health_app req;
		struct app_slot *sl;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
			ret = -EFAULT;
			break;
		}
		sl = app_find(req.app_key, req.uid);
		if (!sl) {
			ret = -ENOENT;
			break;
		}
		req.fg_ms = sl->fg_ms;
		req.last_active_ms = sl->last_active_ms;
		memcpy(req.day_ms, sl->day_ms, sizeof(req.day_ms));
		strscpy(req.id, sl->id, FOS_HEALTH_NAME_MAX);
		if (copy_to_user((void __user *)arg, &req, sizeof(req)))
			ret = -EFAULT;
		break;
	}
	case FOS_HEALTH_IOC_ENUM_APP: {
		/* 约定：入参 app_key 传"上一次返回的槽号+1"（从 0 开始），
		 * 出参 app_key 覆盖为槽号，便于家长面板循环快照。 */
		struct fos_health_app req;
		int start, i;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
			ret = -EFAULT;
			break;
		}
		start = (int)req.app_key;
		if (start < 0 || start >= FOS_HEALTH_APP_MAX) {
			ret = -EINVAL;
			break;
		}
		for (i = start; i < FOS_HEALTH_APP_MAX; i++) {
			if (!apps[i].used)
				continue;
			req.app_key = i;          /* 槽号回填 */
			req.uid = apps[i].uid;
			req.fg_ms = apps[i].fg_ms;
			req.last_active_ms = apps[i].last_active_ms;
			memcpy(req.day_ms, apps[i].day_ms, sizeof(req.day_ms));
			strscpy(req.id, apps[i].id, FOS_HEALTH_NAME_MAX);
			if (copy_to_user((void __user *)arg, &req, sizeof(req)))
				ret = -EFAULT;
			goto enum_done;
		}
		ret = -ENOENT;
enum_done:
		break;
	}
	case FOS_HEALTH_IOC_BLOCK: {
		struct fos_health_block req;
		int i, free_i = -1;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
			ret = -EFAULT;
			break;
		}
		if (!req.app_key && req.id[0])
			req.app_key = app_hash(req.id);
		if (!req.app_key) {
			ret = -EINVAL;
			break;
		}
		if (block_find(req.app_key, req.uid))
			break;                    /* 幂等：已在名单 */
		for (i = 0; i < FOS_HEALTH_BLOCK_MAX; i++) {
			if (!blocks[i].used) {
				free_i = i;
				break;
			}
		}
		if (free_i < 0) {
			ret = -ENOSPC;
			break;
		}
		blocks[free_i].used = true;
		blocks[free_i].app_key = req.app_key;
		blocks[free_i].uid = req.uid;
		blocks[free_i].level = req.level;
		strscpy(blocks[free_i].id, req.id, FOS_HEALTH_NAME_MAX);
		break;
	}
	case FOS_HEALTH_IOC_UNBLOCK: {
		struct fos_health_block req;
		struct block_slot *bs;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
			ret = -EFAULT;
			break;
		}
		if (!req.app_key && req.id[0])
			req.app_key = app_hash(req.id);   /* 与 BLOCK 对称 */
		bs = block_find(req.app_key, req.uid);
		if (!bs) {
			ret = -ENOENT;
			break;
		}
		memset(bs, 0, sizeof(*bs));
		break;
	}
	case FOS_HEALTH_IOC_IS_BLOCKED: {
		struct fos_health_block req;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
			ret = -EFAULT;
			break;
		}
		if (!req.app_key && req.id[0])
			req.app_key = app_hash(req.id);   /* 与 BLOCK 对称 */
		ret = block_find(req.app_key, req.uid) ? 0 : -ENOENT;
		break;
	}
	case FOS_HEALTH_IOC_RESET:
		memset(apps, 0, sizeof(apps));
		break;
	default:
		ret = -ENOTTY;
	}

	mutex_unlock(&health_lock);
	return ret;
}

static const struct file_operations health_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = health_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/* ──────────────────────── /proc/fos_health ────────────────── */

static int health_show(struct seq_file *m, void *v)
{
	int i, nb = 0;

	mutex_lock(&health_lock);
	for (i = 0; i < FOS_HEALTH_BLOCK_MAX; i++)
		if (blocks[i].used)
			nb++;
	seq_printf(m, "# fos_health: apps=%zu blocklist=%d today=%u\n",
		   ARRAY_SIZE(apps), nb, today_epoch_day());
	for (i = 0; i < FOS_HEALTH_APP_MAX; i++) {
		if (!apps[i].used)
			continue;
		seq_printf(m, "uid=%-6u key=%-10u total=%10llu ms | %s\n",
			   apps[i].uid, apps[i].app_key, apps[i].fg_ms,
			   apps[i].id);
	}
	seq_puts(m, "-- blocked --\n");
	for (i = 0; i < FOS_HEALTH_BLOCK_MAX; i++) {
		if (!blocks[i].used)
			continue;
		seq_printf(m, "uid=%-6u key=%-10u level=%u | %s\n",
			   blocks[i].uid, blocks[i].app_key, blocks[i].level,
			   blocks[i].id);
	}
	mutex_unlock(&health_lock);
	return 0;
}

static int health_seq_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, health_show, NULL);
}

static const struct proc_ops health_proc_ops = {
	.proc_open    = health_seq_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ───────────────────────── 模块出入口 ─────────────────────── */

static int __init health_usage_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&dev_no, 0, 1, DRIVER_NAME);
	if (ret)
		return ret;

	health_class = class_create("fos_health");
	if (IS_ERR(health_class)) {
		ret = PTR_ERR(health_class);
		goto err_region;
	}

	cdev_init(&health_cdev, &health_fops);
	health_cdev.owner = THIS_MODULE;
	ret = cdev_add(&health_cdev, dev_no, 1);
	if (ret)
		goto err_class;

	health_device = device_create(health_class, NULL, dev_no, NULL,
				      "fos_health");
	if (IS_ERR(health_device)) {
		ret = PTR_ERR(health_device);
		goto err_cdev;
	}

	if (!proc_create("fos_health", 0444, NULL, &health_proc_ops)) {
		ret = -ENOMEM;
		goto err_device;
	}

	pr_info("fos_health: /dev/fos_health ready (apps=%d blocks=%d)\n",
		FOS_HEALTH_APP_MAX, FOS_HEALTH_BLOCK_MAX);
	return 0;

err_device:
	device_destroy(health_class, dev_no);
err_cdev:
	cdev_del(&health_cdev);
err_class:
	class_destroy(health_class);
err_region:
	unregister_chrdev_region(dev_no, 1);
	return ret;
}

static void __exit health_usage_exit(void)
{
	remove_proc_entry("fos_health", NULL);
	device_destroy(health_class, dev_no);
	cdev_del(&health_cdev);
	class_destroy(health_class);
	unregister_chrdev_region(dev_no, 1);
	pr_info("fos_health: unloaded\n");
}

module_init(health_usage_init);
module_exit(health_usage_exit);
