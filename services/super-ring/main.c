// SPDX-License-Identifier: MIT
/*
 * fos-super-ring-d — 超级环：全局设备协同入口（参考实现）
 *
 * 设备全景视图的服务端。职责：
 *   1. 接收发现层（mDNS/BLE/星闪转译）的上线心跳，维护设备表；
 *   2. 登记投屏/互传/剪贴板/网络共享/控制五类会话；
 *   3. 若内核 super_ring 模块在用（/dev/fos_ring 存在），把设备表
 *      同步进内核，让任何本机进程 cat /proc/fos_ring 都能看到；
 *      模块缺失时优雅降级为纯内存视图 —— 开轮版"可替换"约定。
 *
 * 数据流不参与：投屏流与文件分片由应用直连，本服务只发「会话凭证」。
 */
#include "../common/fos_daemon.h"
#include "../common/fos_json.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* 与 kernel/super_ring/super_ring.h 相同的布局（不 include 内核头，
 * 保持用户态独立编译；ABI 由契约测试保障）。
 * ★ 字段增删必须与内核头同步：ioctl 命令号编码了结构体尺寸，
 *   两侧尺寸一差，REG_DEV 会直接 -ENOTTY。 */
struct ring_dev_pub {
	uint8_t type, link, online, reserved;
	uint64_t caps, last_seen_ms;
	uint32_t id, rssi;
	char name[32];
	char type_str[16];
	char caps_str[8][16];
};

#define RING_IOC_MAGIC 'R'
#define RING_REG_DEV   _IOWR(RING_IOC_MAGIC, 1, struct ring_dev_pub)
#define RING_UNREG_DEV _IOW(RING_IOC_MAGIC, 2, uint32_t)

#define MAX_DEV  64
#define MAX_SESS 128

/* 设备类型编号：与内核侧 enum 一致 */
static const char *const TYPE_NAMES[] = {
	"phone", "tablet", "pc", "tv", "watch", "bt_audio", "iot"
};
static const char *const LINK_NAMES[] = {
	"-", "lan", "p2p", "bt", "btle", "nearlink", "usb"
};
static const char *const KIND_NAMES[] = {
	"cast", "xfer", "clip", "net", "ctrl"
};

struct dev {
	int used;
	uint32_t id;             /* 本地自增，0 非法 */
	int type, link;
	char name[32];
	uint64_t caps;
	time_t seen;
};

struct sess {
	int used;
	uint32_t id;
	int kind;
	uint32_t src, dst;
};

static struct dev devices[MAX_DEV];
static struct sess sessions[MAX_SESS];
static uint32_t next_dev = 1, next_sess = 1;
static int kernel_fd = -1;      /* /dev/fos_ring；-1 表示纯内存模式 */

static void kernel_sync_register(const struct dev *d)
{
	struct ring_dev_pub pub;

	if (kernel_fd < 0)
		return;
	memset(&pub, 0, sizeof(pub));
	pub.type = (uint8_t)d->type;
	pub.link = (uint8_t)d->link;
	pub.caps = d->caps;
	snprintf(pub.name, sizeof(pub.name), "%s", d->name);
	snprintf(pub.type_str, sizeof(pub.type_str), "%s",
		 TYPE_NAMES[d->type]);
	if (ioctl(kernel_fd, RING_REG_DEV, &pub) == 0) {
		/* 内核 id 与内存 id 对齐：登记失败仅告警，视图照常工作 */
	} else {
		LOGW("super-ring", "内核同步失败(降级内存模式): %s",
		     strerror(errno));
		close(kernel_fd);
		kernel_fd = -1;
	}
}

static int dev_init(void *user)
{
	(void)user;
	kernel_fd = open("/dev/fos_ring", O_RDWR | O_CLOEXEC);
	if (kernel_fd >= 0)
		LOGI("super-ring", "内核注册表已接入 (/dev/fos_ring)");
	else
		LOGI("super-ring", "内核模块未加载，使用进程内存视图");
	return 0;
}

static void dev_fini(void *user)
{
	(void)user;
	if (kernel_fd >= 0)
		close(kernel_fd);
}

/* ── 表操作（线性查找，参考实现的克制风格）── */

static struct dev *dev_find(uint32_t id)
{
	for (int i = 0; i < MAX_DEV; i++)
		if (devices[i].used && devices[i].id == id)
			return &devices[i];
	return NULL;
}

static struct dev *dev_find_name(const char *name)
{
	for (int i = 0; i < MAX_DEV; i++)
		if (devices[i].used && strcmp(devices[i].name, name) == 0)
			return &devices[i];
	return NULL;
}

static struct dev *dev_upsert(const char *name, int type, int link,
			      uint64_t caps)
{
	struct dev *d = dev_find_name(name);

	if (d) {
		d->seen = time(NULL);
		d->caps = caps;
		return d;
	}
	for (int i = 0; i < MAX_DEV; i++) {
		if (devices[i].used)
			continue;
		d = &devices[i];
		memset(d, 0, sizeof(*d));
		d->used = 1;
		d->id = next_dev++;
		snprintf(d->name, sizeof(d->name), "%s", name);
		d->type = type;
		d->link = link;
		d->caps = caps;
		d->seen = time(NULL);
		kernel_sync_register(d);
		return d;
	}
	return NULL;   /* 表满：调用方回 -ENOSPC */
}

static void dev_down(uint32_t id)
{
	struct dev *d = dev_find(id);

	if (!d)
		return;
	if (kernel_fd >= 0) {
		uint32_t kid = id;

		ioctl(kernel_fd, RING_UNREG_DEV, &kid);
	}
	memset(d, 0, sizeof(*d));
}

/* 辅助：枚举字符串 → 编号 */
static int name_to_idx(const char *const *arr, size_t n, const char *v,
		       int def)
{
	for (size_t i = 0; i < n; i++)
		if (strcmp(arr[i], v) == 0)
			return (int)i;
	return def;
}
#define STR2IDX(arr, v) name_to_idx(arr, sizeof(arr) / sizeof(arr[0]), v, 0)

/* ── 命令实现 ── */

/* devices.list —— 设备全景视图 */
static int cmd_devices_list(const fos_jobj *req, fos_jbuf *b,
			    char *em, size_t esz, void *user)
{
	(void)req; (void)em; (void)esz; (void)user;
	int now = 0;

	fj_begin_arr(b, "devices");
	for (int i = 0; i < MAX_DEV; i++) {
		if (!devices[i].used)
			continue;
		now++;
		fj_arr_obj(b);
		fj_kv_i64(b, "id", devices[i].id);
		fj_kv_str(b, "name", devices[i].name);
		fj_kv_str(b, "type", TYPE_NAMES[devices[i].type]);
		fj_kv_str(b, "link", LINK_NAMES[devices[i].link]);
		fj_kv_i64(b, "caps", (int64_t)devices[i].caps);
		fj_kv_i64(b, "age_s", (int64_t)(time(NULL) - devices[i].seen));
		fj_end_obj(b);
	}
	fj_end_arr(b);
	fj_kv_i64(b, "online", now);

	fj_begin_arr(b, "sessions");
	for (int i = 0; i < MAX_SESS; i++) {
		if (!sessions[i].used)
			continue;
		fj_arr_obj(b);
		fj_kv_i64(b, "id", sessions[i].id);
		fj_kv_str(b, "kind", KIND_NAMES[sessions[i].kind]);
		fj_kv_i64(b, "src", sessions[i].src);
		fj_kv_i64(b, "dst", sessions[i].dst);
		fj_end_obj(b);
	}
	fj_end_arr(b);
	return 0;
}

/* device.heartbeat {name,type,link,caps} —— 发现层上线/心跳 */
static int cmd_device_heartbeat(const fos_jobj *req, fos_jbuf *b,
				char *em, size_t esz, void *user)
{
	(void)user;
	char name[32], type[16] = "phone", link[16] = "lan";

	fos_json_get_str(req, "name", "", name, sizeof(name));
	fos_json_get_str(req, "type", "phone", type, sizeof(type));
	fos_json_get_str(req, "link", "lan", link, sizeof(link));
	if (!name[0]) {
		snprintf(em, esz, "缺少 name");
		return -32602;
	}
	struct dev *d = dev_upsert(name, STR2IDX(TYPE_NAMES, type),
				   STR2IDX(LINK_NAMES, link),
				   (uint64_t)fos_json_get_int(req, "caps", 0));
	if (!d) {
		snprintf(em, esz, "设备表已满");
		return -32000;
	}
	fj_kv_i64(b, "id", d->id);
	fj_kv_bool(b, "new", d->seen == time(NULL));
	return 0;
}

/* device.drop {id} —— 下线 */
static int cmd_device_drop(const fos_jobj *req, fos_jbuf *b,
			   char *em, size_t esz, void *user)
{
	(void)b; (void)user;
	uint32_t id = (uint32_t)fos_json_get_int(req, "id", 0);

	if (!dev_find(id)) {
		snprintf(em, esz, "设备不存在");
		return -32602;
	}
	dev_down(id);
	return 0;
}

/* session.open {kind,src,dst} —— 建立会话，返回会话凭证 */
static int cmd_session_open(const fos_jobj *req, fos_jbuf *b,
			    char *em, size_t esz, void *user)
{
	(void)user;
	char kind[16] = "cast";
	uint32_t src, dst;

	fos_json_get_str(req, "kind", "cast", kind, sizeof(kind));
	src = (uint32_t)fos_json_get_int(req, "src", 0);
	dst = (uint32_t)fos_json_get_int(req, "dst", 0);

	if (!dev_find(src)) {
		snprintf(em, esz, "发起方不在线");
		return -32602;
	}
	if (dst && !dev_find(dst)) {
		snprintf(em, esz, "接收方不在线");
		return -32602;
	}
	for (int i = 0; i < MAX_SESS; i++) {
		if (sessions[i].used)
			continue;
		sessions[i].used = 1;
		sessions[i].id = next_sess++;
		sessions[i].kind = STR2IDX(KIND_NAMES, kind);
		sessions[i].src = src;
		sessions[i].dst = dst;
		fj_kv_i64(b, "session", sessions[i].id);
		/* 真实系统在此返回一次性 token（HMAC），两端凭 token 直连 */
		fj_kv_str(b, "token_note", "参考实现：直连需自行预配对");
		return 0;
	}
	snprintf(em, esz, "会话已满");
	return -32000;
}

/* session.close {id} */
static int cmd_session_close(const fos_jobj *req, fos_jbuf *b,
			     char *em, size_t esz, void *user)
{
	(void)b; (void)user;
	uint32_t id = (uint32_t)fos_json_get_int(req, "id", 0);

	for (int i = 0; i < MAX_SESS; i++) {
		if (sessions[i].used && sessions[i].id == id) {
			memset(&sessions[i], 0, sizeof(sessions[i]));
			return 0;
		}
	}
	snprintf(em, esz, "会话不存在");
	return -32602;
}

static const fos_cmd_table TABLE[] = {
	{ "devices.list",     cmd_devices_list,     "设备全景视图" },
	{ "device.heartbeat", cmd_device_heartbeat, "上线/心跳登记" },
	{ "device.drop",      cmd_device_drop,      "设备下线" },
	{ "session.open",     cmd_session_open,     "建立协同会话" },
	{ "session.close",    cmd_session_close,    "结束会话" },
	{ NULL, NULL, NULL }
};

int main(int argc, char **argv)
{
	return fos_daemon_main(argc, argv, "super-ring", TABLE,
			       dev_init, dev_fini);
}
