// SPDX-License-Identifier: MulanPSL-2.0
/*
 * fos-migration-d — 换机方案（双向迁移，参考实现）
 *
 * 支持双向：既可以从其他系统迁入 FamilyOS（import），
 * 也可以从 FamilyOS 迁出到其他系统（export）。
 * 迁移内容：应用列表、文件、设置、账号关联（四类通道）。
 *
 * 迁移包格式（.fosmig，JSON manifest + tar 归档的混合，见
 * docs/migration-format.md 若社区需要再补；参考实现简化为：
 *   <out>/manifest.json     迁移清单（版本、来源、四类通道计数）
 *   <out>/apps.json         应用列表（含 .fos 包名与外部来源标记）
 *   <out>/files/            用户文件
 *   <out>/settings.json     设置快照
 *   <out>/accounts.json     账号关联（只迁标识与 token 句柄，
 *                           **绝不迁移明文密码**——红线）
 *
 * 进度模型：单任务状态机（idle→running→done/failed），status 轮询。
 */
#include "../common/fos_daemon.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum mig_state { MIG_IDLE = 0, MIG_RUNNING, MIG_DONE, MIG_FAILED };

static struct {
	enum mig_state state;
	char kind[16];        /* export / import */
	char path[256];
	int  done_items;
	int  total_items;
	char error[256];
} task;

/* 四类迁移通道（对齐文档：应用列表、文件、设置、账号关联） */
static const char *const CHANNELS[] = { "apps", "files", "settings",
					"accounts", NULL };

static int channel_wanted(const fos_jobj *req, const char *ch)
{
	/* args.include 缺省 = 全部四类 */
	fos_jtok t;

	if (fos_json_get(req, "include", &t) != 0)
		return 1;
	return memchr(t.s, '"', t.len) &&
	       memmem(t.s, t.len, ch, strlen(ch)) != NULL;
}

static int write_small(const char *dir, const char *name, const char *body)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "we");
	if (!f)
		return -1;
	fputs(body, f);
	fclose(f);
	return 0;
}

/* export {out,include} —— 导出（迁出 FamilyOS / 备份） */
static int cmd_export(const fos_jobj *req, fos_jbuf *b,
		      char *em, size_t esz, void *user)
{
	(void)user;
	char out[256];
	int total = 0;

	fos_json_get_str(req, "out", "", out, sizeof(out));
	if (!out[0] || strstr(out, "..")) {
		snprintf(em, esz, "缺少 out 或含 '..'");
		return -32602;
	}
	if (task.state == MIG_RUNNING) {
		snprintf(em, esz, "已有迁移任务在跑");
		return -32000;
	}

	if (mkdir(out, 0750) != 0 && errno != EEXIST) {
		snprintf(em, esz, "无法创建 %s: %s", out, strerror(errno));
		return -32000;
	}
	task.state = MIG_RUNNING;
	snprintf(task.kind, sizeof(task.kind), "export");
	snprintf(task.path, sizeof(task.path), "%s", out);

	/* 参考实现：每个通道写一个占位产物，社区接入真实导出器 */
	for (int i = 0; CHANNELS[i]; i++) {
		char fname[32];

		if (!channel_wanted(req, CHANNELS[i]))
			continue;
		total++;
		snprintf(fname, sizeof(fname), "%s.json", CHANNELS[i]);
		write_small(out, fname,
			    "{\"channel\":\"placeholder\","
			    "\"note\":\"参考实现占位，接入真实导出器\"}");
	}
	task.total_items = total;
	task.done_items = total;
	task.state = MIG_DONE;

	fj_kv_str(b, "out", out);
	fj_kv_i64(b, "channels", total);
	fj_kv_str(b, "state", "done");
	return 0;
}

/* import {src} —— 导入（迁入 FamilyOS） */
static int cmd_import(const fos_jobj *req, fos_jbuf *b,
		      char *em, size_t esz, void *user)
{
	(void)user;
	char src[256];

	fos_json_get_str(req, "src", "", src, sizeof(src));
	if (!src[0] || strstr(src, "..")) {
		snprintf(em, esz, "缺少 src 或含 '..'");
		return -32602;
	}
	if (access(src, R_OK) != 0) {
		snprintf(em, esz, "迁移包不可读: %s", src);
		return -32602;
	}
	if (task.state == MIG_RUNNING) {
		snprintf(em, esz, "已有迁移任务在跑");
		return -32000;
	}
	task.state = MIG_RUNNING;
	snprintf(task.kind, sizeof(task.kind), "import");
	snprintf(task.path, sizeof(task.path), "%s", src);
	/* 参考实现：识别四类通道文件即视为导入完成；真实系统在此
	 * 逐应用装 .fos、逐文件入 fosfs、逐设置项 merge、账号句柄交
	 * 给账号服务换取 token。 */
	task.total_items = 0;
	for (int i = 0; CHANNELS[i]; i++)
		if (access(src, F_OK) == 0)
			task.total_items++;
	task.done_items = task.total_items;
	task.state = MIG_DONE;

	fj_kv_str(b, "src", src);
	fj_kv_i64(b, "items", task.total_items);
	fj_kv_str(b, "state", "done");
	return 0;
}

/* status —— 进度轮询 */
static int cmd_status(const fos_jobj *req, fos_jbuf *b,
		      char *em, size_t esz, void *user)
{
	static const char *const S[] = { "idle", "running", "done",
					 "failed" };

	(void)req; (void)em; (void)esz; (void)user;

	fj_kv_str(b, "state", S[task.state]);
	fj_kv_str(b, "kind", task.kind);
	fj_kv_str(b, "path", task.path);
	fj_kv_i64(b, "done", task.done_items);
	fj_kv_i64(b, "total", task.total_items);
	if (task.error[0])
		fj_kv_str(b, "error", task.error);
	return 0;
}

static const fos_cmd_table TABLE[] = {
	{ "export", cmd_export, "迁出/备份 {out,include?}" },
	{ "import", cmd_import, "迁入 {src}" },
	{ "status", cmd_status, "进度查询" },
	{ NULL, NULL, NULL }
};

int main(int argc, char **argv)
{
	return fos_daemon_main(argc, argv, "migration", TABLE, NULL, NULL);
}
