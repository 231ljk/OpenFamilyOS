/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * super_ring.h — 超级环（Super Ring）内核/用户态接口契约
 *
 * 超级环是 FamilyOS 的全局设备协同入口：账号下所有在线设备在这里
 * 注册；投屏、文件互传、剪贴板同步、网络共享登记为「会话」；
 * 非本生态的蓝牙外设（耳机、音箱等）同样作为设备登记，接受控制。
 *
 * 用户态通过 /dev/fos_ring 上的 ioctl 访问；本头文件是 ABI 契约：
 *   - 结构体只增字段、不改语义；
 *   - 命令编号只顺延、不复用。
 */
#ifndef _UAPI_FOS_SUPER_RING_H
#define _UAPI_FOS_SUPER_RING_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define FOS_RING_NAME_MAX   32   /* 设备显示名（UTF-8 截断） */
#define FOS_RING_TYPE_MAX   16   /* 设备类型字符串 */
#define FOS_RING_CAPS_MAX   8    /* 能力标签数量 */
#define FOS_RING_CAP_LEN    16   /* 单个能力标签长度 */
#define FOS_RING_MAX_DEV    64   /* 注册表容量（参考实现，可替换） */
#define FOS_RING_MAX_SESS   128  /* 并发会话上限 */

/* 设备类型：与 services/super-ring 的 JSON 表示保持一致 */
enum fos_ring_dev_type {
	FOS_DEV_PHONE = 0,
	FOS_DEV_TABLET,
	FOS_DEV_PC,
	FOS_DEV_TV,
	FOS_DEV_WATCH,
	FOS_DEV_BT_AUDIO,   /* 非本生态蓝牙外设：耳机/音箱 */
	FOS_DEV_IOT,      /* 智能家居桥接后登记的终端 */
	__FOS_DEV_MAX,
};

/* 能力位：设备声明自己支持什么，由超级环 UI 层决定呈现什么 */
#define FOS_CAP_CAST        (1U << 0)  /* 被投屏 */
#define FOS_CAP_RECEIVE     (1U << 1)  /* 接收投屏 */
#define FOS_CAP_FILE_XFER   (1U << 2)  /* 文件互传 */
#define FOS_CAP_CLIPBOARD   (1U << 3)  /* 剪贴板同步 */
#define FOS_CAP_NET_SHARE   (1U << 4)  /* 网络共享 */
#define FOS_CAP_CONTROL     (1U << 5)  /* 可被跨端控制（语音助手/遥控器） */
#define FOS_CAP_AUDIO_OUT   (1U << 6)  /* 音频输出（蓝牙外设常见） */
#define FOS_CAP_HOME_CTRL   (1U << 7)  /* 智能家居被控（灯/空调等） */

/* 连接通道 */
enum fos_ring_link {
	FOS_LINK_NONE = 0,
	FOS_LINK_LAN,        /* 局域网（mDNS/SSDP 发现） */
	FOS_LINK_P2P_WIFI,   /* Wi-Fi 直连 */
	FOS_LINK_BT,         /* 经典蓝牙 */
	FOS_LINK_BTLE,       /* 蓝牙低功耗 */
	FOS_LINK_NEARLINK,   /* 星闪（桥接层转译后登记） */
	FOS_LINK_USB,        /* USB 调试/数据线 */
	__FOS_LINK_MAX,
};

/*
 * 设备登记条目。用户态守护进程（fos-super-ringd）在设备上线/下线时
 * 负责填充并注册/注销；内核只保存权威视图，供任意本地进程查询。
 */
struct fos_ring_device {
	__u8  type;                       /* enum fos_ring_dev_type */
	__u8  link;                       /* enum fos_ring_link */
	__u8  online;                     /* 1=在线 0=离线（离线也保留一段时间） */
	__u8  reserved;
	__u64 caps;                       /* FOS_CAP_* 位掩码 */
	__u64 last_seen_ms;               /* 单调时钟毫秒，用于在线判定 */
	__u32 id;                         /* 内核分配的注册 id（0 非法） */
	__u32 rssi;                       /* 发现信号强度（有则填，0=未知） */
	char  name[FOS_RING_NAME_MAX];    /* 显示名 */
	char  type_str[FOS_RING_TYPE_MAX];/* 原始类型串，如 "watch" */
	char  caps_str[FOS_RING_CAPS_MAX][FOS_RING_CAP_LEN]; /* 可读能力标签 */
};

/* 会话登记：一次投屏 / 一次互传 / 一轮剪贴板同步都算一个会话 */
enum fos_ring_session_kind {
	FOS_SESS_CAST = 0,
	FOS_SESS_FILE_XFER,
	FOS_SESS_CLIPBOARD,
	FOS_SESS_NET_SHARE,
	FOS_SESS_CONTROL,   /* 跨端控制（语音助手/遥控） */
	__FOS_SESS_MAX,
};

struct fos_ring_session {
	__u32 id;         /* 内核分配 */
	__u32 kind;       /* enum fos_ring_session_kind */
	__u32 src_dev;    /* 发起方设备 id */
	__u32 dst_dev;    /* 接收方设备 id（0=广播型，如网络共享） */
	__u32 pid;        /* 发起会话的用户态进程 pid（用于崩溃自动回收） */
	__u64 started_ms; /* 开始时间（单调） */
};

/* 会话创建请求：用户态填入 src/dst/kind，id 由内核回填 */
struct fos_ring_sess_req {
	__u32 kind;
	__u32 src_dev;
	__u32 dst_dev;
	__u32 id;         /* out */
};

#define FOS_RING_IOC_MAGIC 'R'

/* 注册/更新设备：入参 struct fos_ring_device，内核回填/分配 id */
#define FOS_RING_IOC_REG_DEV   _IOWR(FOS_RING_IOC_MAGIC, 1, struct fos_ring_device)
/* 注销设备：入参 __u32 id */
#define FOS_RING_IOC_UNREG_DEV _IOW(FOS_RING_IOC_MAGIC, 2, __u32)
/* 查询单个设备：入参 struct fos_ring_device，按 id 或 name 匹配，-ENOENT 未找到 */
#define FOS_RING_IOC_GET_DEV   _IOWR(FOS_RING_IOC_MAGIC, 3, struct fos_ring_device)
/* 打开会话：入参 struct fos_ring_sess_req，回填 id */
#define FOS_RING_IOC_SESS_OPEN _IOWR(FOS_RING_IOC_MAGIC, 4, struct fos_ring_sess_req)
/* 关闭会话：入参 __u32 sess id */
#define FOS_RING_IOC_SESS_CLOSE _IOW(FOS_RING_IOC_MAGIC, 5, __u32)
/* 注册表快照数量：出参 __u32（配合 /proc/fos/ring 使用） */
#define FOS_RING_IOC_COUNT     _IOR(FOS_RING_IOC_MAGIC, 6, __u32)

#endif /* _UAPI_FOS_SUPER_RING_H */
