// SPDX-License-Identifier: MIT
/*
 * fos-health-d — 健康使用设备（家长管理，参考实现）
 *
 * 能力（开发文档 第七章）：
 *   - 实时查看应用使用情况：用了哪些应用、使用了多少天；
 *   - 禁用应用：家长可随时禁用指定应用；
 *   - 覆盖电脑、平板、手机、电视、手表全设备（数据经超级环聚合）。
 *
 * 架构分工：
 *   内核 health_usage 模块 = 权威计时 + 名单存放（/dev/fos_health）；
 *   本守护进程 = 名单持久化、拦截执行（桌面/包管理器调用 can.launch）、
 *                家长面板的 IPC 门面。
 *   内核模块缺失时降级为纯内存 + JSON 落盘（dev 环境可用，与 super-ring
 *   同样的「可替换」约定）。
 *
 * 持久化：<FOS_DATA_DIR 或 /var/lib/fos>/health.json
 */
#include "../common/fos_daemon.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* 与 kernel/health_usage/health_usage.h 同布局（不 include 内核头） */
struct kh_app {
	uint32_t app_key, uid;
	union {
		uint64_t fg_ms;
		uint64_t fg_ms_delta;
	};
	uint64_t last_active_ms;
	uint64_t day_ms[7];
	char id[64];
};

struct kh_block {
	uint32_t app_key, uid;
	uint8_t level, reserved;
	char id[64];
};

#define H_IOC_MAGIC 'H'
#define H_REGISTER   _IOWR(H_IOC_MAGIC, 1, struct kh_app)
#define H_ACCOUNT    _IOW(H_IOC_MAGIC, 2, struct kh_app)
#define H_BLOCK      _IOW(H_IOC_MAGIC, 5, struct kh_block)
#define H_UNBLOCK    _IOW(H_IOC_MAGIC, 6, struct kh_block)

#define MAX_REC 256

struct usage_rec {
	int used;
	char pkg[64];
	uint32_t uid;
	uint64_t total_ms;
	uint64_t day_ms[7];     /* 滚动 7 日（周一起始简化为索引 0..6） */
	time_t first_seen;      /* 「使用了多少天」的依据 */
	int blocked;
	int block_level;        /* 0=可询问 1=硬禁用 */
};

static struct usage_rec recs[MAX_REC];
static int kernel_fd = -1;
static char data_path[512];

/* ── 落盘 ── */

static void save(void)
{
	char tmp[560];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.tmp", data_path);
	f = fopen(tmp, "we");
	if (!f) {
		LOGW("health", "无法写 %s: %s", tmp, strerror(errno));
		return;
	}
	fputs("{\n  \"version\": 1,\n  \"apps\": [\n", f);
	for (int i = 0, first = 1; i < MAX_REC; i++) {
		if (!recs[i].used)
			continue;
		fprintf(f, "%s    {\"pkg\":\"%s\",\"uid\":%u,"
			"\"total_ms\":%llu,\"first_seen\":%lld,"
			"\"blocked\":%d,\"level\":%d}",
			first ? "" : ",\n", recs[i].pkg, recs[i].uid,
			(unsigned long long)recs[i].total_ms,
			(long long)recs[i].first_seen, recs[i].blocked,
			recs[i].block_level);
		first = 0;
	}
	fputs("\n  ]\n}\n", f);
	fclose(f);
	if (rename(tmp, data_path) != 0)
		LOGW("health", "落盘 rename 失败: %s", strerror(errno));
}

/* 极简恢复：只认本服务自己写的格式（"pkg":... 逐行解析） */
static void load(void)
{
	FILE *f = fopen(data_path, "re");
	char line[256];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		struct usage_rec *r = NULL;
		char pkg[64];
		unsigned uid;
		unsigned long long total;
		long long fs;
		int blk, lvl;

		if (sscanf(line, "    {\"pkg\":\"%63[^\"]\",\"uid\":%u,"
			     "\"total_ms\":%llu,\"first_seen\":%lld,"
			     "\"blocked\":%d,\"level\":%d}",
			   pkg, &uid, &total, &fs, &blk, &lvl) == 6) {
			for (int i = 0; i < MAX_REC; i++) {
				if (!recs[i].used) {
					r = &recs[i];
					break;
				}
			}
			if (!r)
				break;
			r->used = 1;
			snprintf(r->pkg, sizeof(r->pkg), "%s", pkg);
			r->uid = uid;
			r->total_ms = total;
			r->first_seen = fs;
			r->blocked = blk;
			r->block_level = lvl;
		}
	}
	fclose(f);
}

/* ── 记录表 ── */

static struct usage_rec *rec_find(const char *pkg, uint32_t uid)
{
	for (int i = 0; i < MAX_REC; i++)
		if (recs[i].used && recs[i].uid == uid &&
		    strcmp(recs[i].pkg, pkg) == 0)
			return &recs[i];
	return NULL;
}

static struct usage_rec *rec_ensure(const char *pkg, uint32_t uid)
{
	struct usage_rec *r = rec_find(pkg, uid);

	if (r)
		return r;
	for (int i = 0; i < MAX_REC; i++) {
		if (recs[i].used)
			continue;
		r = &recs[i];
		memset(r, 0, sizeof(*r));
		r->used = 1;
		snprintf(r->pkg, sizeof(r->pkg), "%s", pkg);
		r->uid = uid;
		r->first_seen = time(NULL);
		return r;
	}
	return NULL;
}

static int days_used(const struct usage_rec *r)
{
	long d = (long)(time(NULL) - r->first_seen) / 86400;

	return (int)(d + 1 > 0 ? d + 1 : 1);
}

/* ── IPC 命令 ── */

static int cmd_usage_list(const fos_jobj *req, fos_jbuf *b,
			  char *em, size_t esz, void *user)
{
	(void)req; (void)em; (void)esz; (void)user;

	fj_begin_arr(b, "apps");
	for (int i = 0; i < MAX_REC; i++) {
		if (!recs[i].used)
			continue;
		fj_arr_obj(b);
		fj_kv_str(b, "pkg", recs[i].pkg);
		fj_kv_i64(b, "uid", recs[i].uid);
		fj_kv_i64(b, "total_ms", (int64_t)recs[i].total_ms);
		fj_kv_i64(b, "days", days_used(&recs[i]));
		fj_kv_bool(b, "blocked", recs[i].blocked);
		fj_end_obj(b);
	}
	fj_end_arr(b);
	fj_kv_str(b, "note", "实时查看应用使用情况：用了哪些应用、使用了多少天");
	return 0;
}

static int cmd_usage_add(const fos_jobj *req, fos_jbuf *b,
			 char *em, size_t esz, void *user)
{
	(void)user;
	char pkg[64];
	uint64_t ms;
	uint32_t uid;
	struct usage_rec *r;

	fos_json_get_str(req, "pkg", "", pkg, sizeof(pkg));
	ms = (uint64_t)fos_json_get_int(req, "ms", 0);
	uid = (uint32_t)fos_json_get_int(req, "uid", 0);
	if (!pkg[0] || ms == 0 || ms > 3600000) {
		snprintf(em, esz, "pkg 缺失或 ms 非法(0<ms<=3600000)");
		return -32602;
	}
	r = rec_ensure(pkg, uid);
	if (!r) {
		snprintf(em, esz, "记录表已满");
		return -32000;
	}
	r->total_ms += ms;

	if (kernel_fd >= 0) {
		struct kh_app ka;

		/* 注意：app_key 一律由内核 REGISTER 依包名计算并回填，
		 * 守护进程不自算哈希，保证与 /proc/fos_health 对齐。 */
		memset(&ka, 0, sizeof(ka));
		ka.uid = uid;
		snprintf(ka.id, sizeof(ka.id), "%s", pkg);
		if (ioctl(kernel_fd, H_REGISTER, &ka) != 0) {
			LOGW("health", "内核登记失败，仅本地记账");
		} else {
			ka.fg_ms_delta = ms;
			if (ioctl(kernel_fd, H_ACCOUNT, &ka) != 0)
				LOGW("health", "内核计数失败");
		}
	}
	save();
	fj_kv_i64(b, "total_ms", (int64_t)r->total_ms);
	return 0;
}

/* block.set {pkg,level,uid?} —— 家长禁用应用 */
static int cmd_block_set(const fos_jobj *req, fos_jbuf *b,
			 char *em, size_t esz, void *user)
{
	(void)user; (void)b;
	char pkg[64];
	int level = (int)fos_json_get_int(req, "level", 1);
	uint32_t uid = (uint32_t)fos_json_get_int(req, "uid", 0);
	struct usage_rec *r;

	fos_json_get_str(req, "pkg", "", pkg, sizeof(pkg));
	if (!pkg[0] || level < 0 || level > 1) {
		snprintf(em, esz, "缺少 pkg 或 level 非法");
		return -32602;
	}
	r = rec_ensure(pkg, uid);
	if (!r) {
		snprintf(em, esz, "记录表已满");
		return -32000;
	}
	r->blocked = 1;
	r->block_level = level;

	if (kernel_fd >= 0) {
		struct kh_block kb;

		memset(&kb, 0, sizeof(kb));
		/* app_key=0：内核依 id 计算，保持与计数器同一哈希 */
		kb.uid = uid;
		kb.level = (uint8_t)level;
		snprintf(kb.id, sizeof(kb.id), "%s", pkg);
		ioctl(kernel_fd, H_BLOCK, &kb);
	}
	save();
	return 0;
}

static int cmd_block_clear(const fos_jobj *req, fos_jbuf *b,
			   char *em, size_t esz, void *user)
{
	(void)user; (void)b;
	char pkg[64];
	uint32_t uid;
	struct usage_rec *r;

	fos_json_get_str(req, "pkg", "", pkg, sizeof(pkg));
	uid = (uint32_t)fos_json_get_int(req, "uid", 0);
	r = rec_find(pkg, uid);
	if (!r || !r->blocked) {
		snprintf(em, esz, "该应用未被禁用");
		return -32602;
	}
	r->blocked = 0;
	if (kernel_fd >= 0) {
		struct kh_block kb;

		memset(&kb, 0, sizeof(kb));
		kb.app_key = (uint32_t)fnv32(pkg);
		kb.uid = uid;
		snprintf(kb.id, sizeof(kb.id), "%s", pkg);
		ioctl(kernel_fd, H_UNBLOCK, &kb);
	}
	save();
	return 0;
}

/* can.launch {pkg,uid} —— 供桌面/包管理器在启动前询问 */
static int cmd_can_launch(const fos_jobj *req, fos_jbuf *b,
			  char *em, size_t esz, void *user)
{
	(void)user;
	char pkg[64];
	uint32_t uid;
	struct usage_rec *r;

	fos_json_get_str(req, "pkg", "", pkg, sizeof(pkg));
	uid = (uint32_t)fos_json_get_int(req, "uid", 0);
	if (!pkg[0]) {
		snprintf(em, esz, "缺少 pkg");
		return -32602;
	}
	r = rec_find(pkg, uid);
	if (r && r->blocked) {
		fj_kv_bool(b, "allow", 0);
		fj_kv_str(b, "reason", r->block_level == 1 ?
			  "家长已禁用（硬禁用）" : "家长已禁用（可询问解锁）");
	} else {
		fj_kv_bool(b, "allow", 1);
	}
	return 0;
}

/* ── 生命周期 ── */

static int init_health(void *user)
{
	const char *dir = getenv("FOS_DATA_DIR");

	(void)user;
	if (dir && *dir)
		snprintf(data_path, sizeof(data_path), "%s/health.json", dir);
	else
		snprintf(data_path, sizeof(data_path),
			 "/var/lib/fos/health.json");
	load();

	kernel_fd = open("/dev/fos_health", O_RDWR | O_CLOEXEC);
	if (kernel_fd >= 0)
		LOGI("health", "内核计数器已接入 (/dev/fos_health)");
	else
		LOGI("health", "内核模块未加载，使用本地记账");

	/* 恢复名单到内核：重启后家长设置必须继续生效 */
	if (kernel_fd >= 0) {
		for (int i = 0; i < MAX_REC; i++) {
			struct kh_block kb;

			if (!recs[i].used || !recs[i].blocked)
				continue;
			memset(&kb, 0, sizeof(kb));
			/* app_key=0：内核按 id 哈希（与计数器同一规则） */
			kb.uid = recs[i].uid;
			kb.level = (uint8_t)recs[i].block_level;
			snprintf(kb.id, sizeof(kb.id), "%s", recs[i].pkg);
			ioctl(kernel_fd, H_BLOCK, &kb);
		}
	}
	return 0;
}

static void fini_health(void *user)
{
	(void)user;
	if (kernel_fd >= 0)
		close(kernel_fd);
}

static const fos_cmd_table TABLE[] = {
	{ "usage.list",   cmd_usage_list,   "应用使用情况总览" },
	{ "usage.add",    cmd_usage_add,    "累计使用时长 {pkg,uid,ms}" },
	{ "block.set",    cmd_block_set,    "禁用应用 {pkg,level,uid?}" },
	{ "block.clear",  cmd_block_clear,  "解除禁用" },
	{ "can.launch",   cmd_can_launch,   "启动前询问 {pkg,uid}" },
	{ NULL, NULL, NULL }
};

int main(int argc, char **argv)
{
	return fos_daemon_main(argc, argv, "health", TABLE,
			       init_health, fini_health);
}
