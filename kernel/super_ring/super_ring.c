// SPDX-License-Identifier: GPL-2.0
/*
 * super_ring.c — FamilyOS 超级环设备注册表（参考实现）
 *
 * 职责边界（功能克制原则）：
 *   内核只保存「谁在线、能干什么、正在开什么会话」的权威视图。
 *   设备发现（mDNS / 蓝牙广播 / 星闪转译）与数据传输（投屏流、
 *   文件分片、剪贴板内容）全部在用户态守护进程 services/super-ring
 *   完成。内核不碰任何业务字节。
 *
 * 接口：
 *   /dev/fos_ring          ioctl（契约见 super_ring.h）
 *   /proc/fos_ring         人类可读快照
 *   /sys/class/fos_ring/fos_ring/stats   计数统计
 *   （各内核模块在 /proc 下使用独立文件，不共用目录，互不依赖加载顺序）
 *
 * 生命周期规则（参考实现，用 kref 而非 RCU）：
 *   - 设备表项被链表持有一份引用；活动会话再各持一份；
 *   - UNREG 只释放「链表持有」那一份：若仍有会话，设备标记 removed
 *     并保留到最后一个会话结束，避免会话指针踩空；
 *   - 守护进程崩溃/退出时，其 fd 关闭会回收它发起的全部会话。
 *
 * 社区可用 RCU + 哈希表 + netlink 事件的高性能版本整体替换本文件，
 * 只要 super_ring.h 的 ioctl 契约不变，上层零改动。
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
#include <linux/list.h>
#include <linux/kref.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/idr.h>

#include "super_ring.h"

#define DRIVER_NAME "fos_super_ring"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("FamilyOS Lab");
MODULE_DESCRIPTION("FamilyOS Super Ring - global device coordination registry");
MODULE_VERSION("0.1");

/* ───────────────────────── 数据结构 ───────────────────────── */

struct ring_dev {
	struct fos_ring_device pub;  /* 对外视图 */
	struct list_head node;       /* 挂到 dev_list */
	struct kref ref;             /* 链表 1 + 每个活动会话 1 */
	bool removed;                /* 已注销但被会话持有 */
};

struct ring_sess {
	struct fos_ring_session pub;
	struct list_head node;       /* 挂到 sess_list */
	struct ring_dev *src;        /* 各持一份引用 */
	struct ring_dev *dst;        /* 可为 NULL（广播型会话） */
};

/* ───────────────────────── 模块状态 ───────────────────────── */

static DEFINE_MUTEX(ring_lock);   /* 粗粒度锁：参考实现够用 */
static LIST_HEAD(dev_list);
static LIST_HEAD(sess_list);
static DEFINE_IDA(dev_ida);
static DEFINE_IDA(sess_ida);
static int dev_count;             /* 链表中的设备数（含被会话持有的） */
static atomic_t sess_live = ATOMIC_INIT(0);

static dev_t dev_no;
static struct class *ring_class;
static struct cdev ring_cdev;
static struct device *ring_device;

/* ───────────────────────── 工具函数 ───────────────────────── */

static u64 mono_ms(void)
{
	return ktime_to_ms(ktime_get());
}

static const char *type_name(u32 t)
{
	static const char *const n[__FOS_DEV_MAX] = {
		[FOS_DEV_PHONE] = "phone", [FOS_DEV_TABLET] = "tablet",
		[FOS_DEV_PC] = "pc", [FOS_DEV_TV] = "tv",
		[FOS_DEV_WATCH] = "watch", [FOS_DEV_BT_AUDIO] = "bt_audio",
		[FOS_DEV_IOT] = "iot",
	};

	return t < __FOS_DEV_MAX ? n[t] : "?";
}

static const char *link_name(u32 l)
{
	static const char *const n[__FOS_LINK_MAX] = {
		[FOS_LINK_NONE] = "-", [FOS_LINK_LAN] = "lan",
		[FOS_LINK_P2P_WIFI] = "p2p", [FOS_LINK_BT] = "bt",
		[FOS_LINK_BTLE] = "btle", [FOS_LINK_NEARLINK] = "nearlink",
		[FOS_LINK_USB] = "usb",
	};

	return l < __FOS_LINK_MAX ? n[l] : "?";
}

static const char *sess_name(u32 k)
{
	static const char *const n[__FOS_SESS_MAX] = {
		[FOS_SESS_CAST] = "cast", [FOS_SESS_FILE_XFER] = "xfer",
		[FOS_SESS_CLIPBOARD] = "clip", [FOS_SESS_NET_SHARE] = "net",
		[FOS_SESS_CONTROL] = "ctrl",
	};

	return k < __FOS_SESS_MAX ? n[k] : "?";
}

/* ───────────────────── 设备表项生命周期 ───────────────────── */

/* 调用方必须持有 ring_lock */
static void ring_dev_release(struct kref *ref)
{
	struct ring_dev *rd = container_of(ref, struct ring_dev, ref);

	list_del(&rd->node);
	dev_count--;
	ida_free(&dev_ida, rd->pub.id);
	kfree(rd);
}

/* 调用方必须持有 ring_lock */
static struct ring_dev *dev_find_locked(u32 id)
{
	struct ring_dev *rd;

	list_for_each_entry(rd, &dev_list, node)
		if (rd->pub.id == id)
			return rd;
	return NULL;
}

/* 调用方必须持有 ring_lock */
static struct ring_dev *dev_find_name_locked(const char *name)
{
	struct ring_dev *rd;

	if (!name[0])
		return NULL;
	list_for_each_entry(rd, &dev_list, node)
		if (strncmp(rd->pub.name, name, FOS_RING_NAME_MAX) == 0)
			return rd;
	return NULL;
}

/* ──────────────────────── 会话操作 ───────────────────────── */

/* 调用方必须持有 ring_lock */
static int sess_open_locked(struct fos_ring_sess_req *req, u32 pid)
{
	struct ring_dev *src, *dst = NULL;
	struct ring_sess *rs;
	int id;

	if (req->kind >= __FOS_SESS_MAX)
		return -EINVAL;

	src = dev_find_locked(req->src_dev);
	if (!src || !src->pub.online)
		return -ENODEV;

	if (req->dst_dev) {
		dst = dev_find_locked(req->dst_dev);
		if (!dst || !dst->pub.online)
			return -ENODEV;
	}

	if (atomic_read(&sess_live) >= FOS_RING_MAX_SESS)
		return -ENOSPC;

	rs = kzalloc(sizeof(*rs), GFP_KERNEL);
	if (!rs)
		return -ENOMEM;

	id = ida_alloc_max(&sess_ida, U32_MAX - 1, GFP_KERNEL);
	if (id < 0) {
		kfree(rs);
		return id;
	}

	rs->pub.kind = req->kind;
	rs->pub.src_dev = src->pub.id;
	rs->pub.dst_dev = dst ? dst->pub.id : 0;
	rs->pub.started_ms = mono_ms();
	rs->pub.pid = pid;
	rs->pub.id = id;

	kref_get(&src->ref);
	rs->src = src;
	if (dst) {
		kref_get(&dst->ref);
		rs->dst = dst;
	}

	list_add_tail(&rs->node, &sess_list);
	atomic_inc(&sess_live);
	req->id = rs->pub.id;   /* 回填给调用方 */
	return 0;
}

/* 调用方必须持有 ring_lock */
static int sess_close_locked(u32 id)
{
	struct ring_sess *rs;

	list_for_each_entry(rs, &sess_list, node) {
		if (rs->pub.id == id) {
			list_del(&rs->node);
			atomic_dec(&sess_live);
			kref_put(&rs->src->ref, ring_dev_release);
			if (rs->dst)
				kref_put(&rs->dst->ref, ring_dev_release);
			ida_free(&sess_ida, rs->pub.id);
			kfree(rs);
			return 0;
		}
	}
	return -ENOENT;
}

/* 按 pid 回收该进程持有的全部会话（fd 关闭时调用） */
static void sess_put_by_pid(u32 pid)
{
	struct ring_sess *rs, *tmp;

	mutex_lock(&ring_lock);
	list_for_each_entry_safe(rs, tmp, &sess_list, node) {
		if (rs->pub.pid == pid) {
			list_del(&rs->node);
			atomic_dec(&sess_live);
			kref_put(&rs->src->ref, ring_dev_release);
			if (rs->dst)
				kref_put(&rs->dst->ref, ring_dev_release);
			ida_free(&sess_ida, rs->pub.id);
			kfree(rs);
		}
	}
	mutex_unlock(&ring_lock);
}

/* ─────────────────────── 设备注册/注销 ─────────────────────── */

/*
 * 注册语义（调用方持锁）：
 *   req.id != 0                  → 更新该 id 的设备（心跳/属性变更）；
 *   req.id == 0 且 name 命中     → 更新的便捷路径；
 *   其余                         → 新建并分配 id。
 * 无论哪种路径，online 置 1、last_seen_ms 刷新、req->id 回填。
 */
static int reg_dev_locked(struct fos_ring_device *req)
{
	struct ring_dev *rd = NULL;

	if (req->type >= __FOS_DEV_MAX)
		return -EINVAL;

	if (req->id)
		rd = dev_find_locked(req->id);
	else
		rd = dev_find_name_locked(req->name);
	if (rd && rd->removed)
		return -ENOENT;   /* 已注销的设备 id 不再接受心跳 */

	if (!rd) {
		int id;

		if (dev_count >= FOS_RING_MAX_DEV)
			return -ENOSPC;
		rd = kzalloc(sizeof(*rd), GFP_KERNEL);
		if (!rd)
			return -ENOMEM;
		id = ida_alloc(&dev_ida, GFP_KERNEL);
		if (id < 0) {
			kfree(rd);
			return id;
		}
		rd->pub.id = id;
		kref_init(&rd->ref);          /* 链表持有 */
		list_add_tail(&rd->node, &dev_list);
		dev_count++;
	}

	rd->pub.type = req->type;
	rd->pub.link = req->link;
	rd->pub.online = 1;
	rd->pub.caps = req->caps;
	rd->pub.rssi = req->rssi;
	strscpy(rd->pub.name, req->name, FOS_RING_NAME_MAX);
	strscpy(rd->pub.type_str, req->type_str, FOS_RING_TYPE_MAX);
	memcpy(rd->pub.caps_str, req->caps_str, sizeof(rd->pub.caps_str));
	rd->pub.last_seen_ms = mono_ms();

	req->id = rd->pub.id;
	return 0;
}

/* 调用方持锁 */
static int unreg_dev_locked(u32 id)
{
	struct ring_dev *rd;

	rd = dev_find_locked(id);
	if (!rd || rd->removed)
		return -ENOENT;

	rd->pub.online = 0;
	rd->removed = true;
	/* 释放链表持有的那一份引用；若还有会话持有，等最后一个会话结束再删 */
	kref_put(&rd->ref, ring_dev_release);
	return 0;
}

/* ──────────────────────── ioctl 分发 ──────────────────────── */

static long ring_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret;

	switch (cmd) {
	case FOS_RING_IOC_REG_DEV: {
		struct fos_ring_device req;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		mutex_lock(&ring_lock);
		ret = reg_dev_locked(&req);
		if (!ret && copy_to_user((void __user *)arg, &req, sizeof(req)))
			ret = -EFAULT;
		mutex_unlock(&ring_lock);
		return ret;
	}
	case FOS_RING_IOC_UNREG_DEV: {
		u32 id;

		if (get_user(id, (u32 __user *)arg))
			return -EFAULT;
		mutex_lock(&ring_lock);
		ret = unreg_dev_locked(id);
		mutex_unlock(&ring_lock);
		return ret;
	}
	case FOS_RING_IOC_GET_DEV: {
		struct fos_ring_device req;
		struct ring_dev *rd;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		mutex_lock(&ring_lock);
		rd = req.id ? dev_find_locked(req.id)
			   : dev_find_name_locked(req.name);
		if (!rd) {
			mutex_unlock(&ring_lock);
			return -ENOENT;
		}
		memcpy(&req, &rd->pub, sizeof(req));
		mutex_unlock(&ring_lock);
		if (copy_to_user((void __user *)arg, &req, sizeof(req)))
			return -EFAULT;
		return 0;
	}
	case FOS_RING_IOC_SESS_OPEN: {
		struct fos_ring_sess_req req;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		mutex_lock(&ring_lock);
		ret = sess_open_locked(&req, task_pid_nr(current));
		if (!ret && copy_to_user((void __user *)arg, &req, sizeof(req)))
			ret = -EFAULT;
		mutex_unlock(&ring_lock);
		return ret;
	}
	case FOS_RING_IOC_SESS_CLOSE: {
		u32 id;

		if (get_user(id, (u32 __user *)arg))
			return -EFAULT;
		mutex_lock(&ring_lock);
		ret = sess_close_locked(id);
		mutex_unlock(&ring_lock);
		return ret;
	}
	case FOS_RING_IOC_COUNT: {
		u32 n;

		mutex_lock(&ring_lock);
		n = dev_count;
		mutex_unlock(&ring_lock);
		if (put_user(n, (u32 __user *)arg))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static int ring_release(struct inode *inode, struct file *filp)
{
	sess_put_by_pid(task_pid_nr(current));
	return 0;
}

static const struct file_operations ring_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = ring_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.release        = ring_release,
};

/* ──────────────────────── /proc/fos/ring ──────────────────── */

static int ring_show(struct seq_file *m, void *v)
{
	struct ring_dev *rd;
	struct ring_sess *rs;

	mutex_lock(&ring_lock);
	seq_printf(m, "devices=%d sessions=%d\n", dev_count,
		   atomic_read(&sess_live));
	list_for_each_entry(rd, &dev_list, node)
		seq_printf(m, "dev %-4u %-8s %-8s %-3s %-20s rssi=%d caps=0x%016llx\n",
			   rd->pub.id, type_name(rd->pub.type),
			   link_name(rd->pub.link),
			   rd->pub.online ? "on" : "off",
			   rd->pub.name, (int)rd->pub.rssi, rd->pub.caps);
	list_for_each_entry(rs, &sess_list, node)
		seq_printf(m, "sess %-4u %-5s src=%u dst=%u pid=%u\n",
			   rs->pub.id, sess_name(rs->pub.kind),
			   rs->pub.src_dev, rs->pub.dst_dev, rs->pub.pid);
	mutex_unlock(&ring_lock);
	return 0;
}

static int ring_seq_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, ring_show, NULL);
}

static const struct proc_ops ring_proc_ops = {
	.proc_open    = ring_seq_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ──────────────────────── sysfs 统计 ──────────────────────── */

static ssize_t stats_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return sysfs_emit(buf, "devices=%d live_sessions=%d\n",
			  READ_ONCE(dev_count), atomic_read(&sess_live));
}
static DEVICE_ATTR_RO(stats);

/* ───────────────────────── 模块出入口 ─────────────────────── */

static int __init super_ring_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&dev_no, 0, 1, DRIVER_NAME);
	if (ret)
		return ret;

	ring_class = class_create("fos_ring");
	if (IS_ERR(ring_class)) {
		ret = PTR_ERR(ring_class);
		goto err_region;
	}

	cdev_init(&ring_cdev, &ring_fops);
	ring_cdev.owner = THIS_MODULE;
	ret = cdev_add(&ring_cdev, dev_no, 1);
	if (ret)
		goto err_class;

	ring_device = device_create(ring_class, NULL, dev_no, NULL, "fos_ring");
	if (IS_ERR(ring_device)) {
		ret = PTR_ERR(ring_device);
		goto err_cdev;
	}

	ret = device_create_file(ring_device, &dev_attr_stats);
	if (ret)
		goto err_device;

	if (!proc_create("fos_ring", 0444, NULL, &ring_proc_ops)) {
		ret = -ENOMEM;
		goto err_stats;
	}

	pr_info("fos_super_ring: /dev/fos_ring ready (max_dev=%d max_sess=%d)\n",
		FOS_RING_MAX_DEV, FOS_RING_MAX_SESS);
	return 0;

err_stats:
	device_remove_file(ring_device, &dev_attr_stats);
err_device:
	device_destroy(ring_class, dev_no);
err_cdev:
	cdev_del(&ring_cdev);
err_class:
	class_destroy(ring_class);
err_region:
	unregister_chrdev_region(dev_no, 1);
	return ret;
}

static void __exit super_ring_exit(void)
{
	struct ring_sess *rs, *rs_tmp;
	struct ring_dev *rd, *rd_tmp;

	remove_proc_entry("fos_ring", NULL);
	device_remove_file(ring_device, &dev_attr_stats);
	device_destroy(ring_class, dev_no);
	cdev_del(&ring_cdev);
	class_destroy(ring_class);
	unregister_chrdev_region(dev_no, 1);

	/* 卸载兜底：先拆会话（会归还设备引用），再拆剩余设备 */
	mutex_lock(&ring_lock);
	list_for_each_entry_safe(rs, rs_tmp, &sess_list, node) {
		list_del(&rs->node);
		kref_put(&rs->src->ref, ring_dev_release);
		if (rs->dst)
			kref_put(&rs->dst->ref, ring_dev_release);
		ida_free(&sess_ida, rs->pub.id);
		kfree(rs);
	}
	list_for_each_entry_safe(rd, rd_tmp, &dev_list, node) {
		list_del(&rd->node);
		dev_count--;
		ida_free(&dev_ida, rd->pub.id);
		kfree(rd);
	}
	mutex_unlock(&ring_lock);

	ida_destroy(&dev_ida);
	ida_destroy(&sess_ida);
	pr_info("fos_super_ring: unloaded\n");
}

module_init(super_ring_init);
module_exit(super_ring_exit);
