// SPDX-License-Identifier: MIT
/*
 * fos-virt-compat-d — 虚拟化兼容层（参考实现）
 *
 * 目标（开发文档 4.4）：基于 Linux 兼容层，支持 ARM64 原生运行与
 * x86_64 指令翻译，让不同架构的应用跨设备运行。
 *   - 电脑端：窗口化并行模式（多窗同开）
 *   - 手机/平板：全屏切换模式（一次一个，前台独占）
 *
 * 真实翻译引擎（Box64/FEX 级别）体量巨大，不在开轮版参考实现里造轮子。
 * 本服务的职责是「调度与决策」：
 *   1. 探测运行架构（uname）→ 决定每个应用走哪条路径：
 *      native（同架构直接 exec）/ translate（异架构交给翻译器后端）/
 *      sandbox（.fos 沙箱包，见 pkg/ 的转包引擎产物）；
 *   2. 管理窗口模式（window=电脑并行 / fullscreen=手机平板切换）；
 *   3. 翻译后端可插拔：按 FOS_TRANSLATOR 环境变量或注册表选择后端
 *      命令模板（社区接 Box64/FEX/qemu-x86_64 都是改配置，不改代码）。
 */
#include "../common/fos_daemon.h"

#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define MAX_APPS 64

struct running {
	int used;
	pid_t pid;
	char pkg[64];
	char mode[64];      /* native / translate:<backend> / sandbox */
};

static struct running apps[MAX_APPS];
static char native_arch[32] = "unknown";
static char win_mode[16] = "window";    /* 默认电脑并行 */

/* 内置后端模板表：命令中的 {PKG_PATH} 由应用路径替换。
 * 真机部署时社区按需覆盖（FOS_TRANSLATOR 环境变量选后端）。 */
struct backend {
	const char *name;
	const char *arch;    /* 该后端能跑的应用架构 */
	const char *cmd;
};

static const struct backend BACKENDS[] = {
	{ "box64",  "x86_64", "box64 {PKG_PATH}" },
	{ "fex",    "x86_64", "fex-interpreter {PKG_PATH}" },
	{ "qemu",   "x86_64", "qemu-x86_64 -L /usr/x86_64-linux-gnu {PKG_PATH}" },
	{ "native", "*",      "{PKG_PATH}" },
	{ NULL, NULL, NULL }
};

static int pick_backend(const char *app_arch)
{
	const char *want = getenv("FOS_TRANSLATOR");
	int fallback = -1, i;

	for (i = 0; BACKENDS[i].name; i++) {
		if (strcmp(BACKENDS[i].arch, app_arch) != 0 &&
		    strcmp(BACKENDS[i].arch, "*") != 0)
			continue;
		if (want && *want && strcmp(want, BACKENDS[i].name) == 0)
			return i;
		if (strcmp(BACKENDS[i].arch, "*") == 0)
			fallback = i;      /* native 兜底放最后 */
		else if (!want || !*want)
			return i;          /* 无偏好：第一个能跑该架构的 */
	}
	return fallback;
}

static void detect_arch(void)
{
	struct utsname u;

	if (uname(&u) == 0) {
		snprintf(native_arch, sizeof(native_arch), "%s", u.machine);
		/* 启发式：非 x86_64 视为手机/平板形态默认全屏。
		 * 真实系统由设备形态服务（devices/）声明。 */
		if (strcmp(u.machine, "x86_64") != 0)
			snprintf(win_mode, sizeof(win_mode), "fullscreen");
	}
}

/* info —— 运行环境与模式 */
static int cmd_info(const fos_jobj *req, fos_jbuf *b,
		    char *em, size_t esz, void *user)
{
	(void)req; (void)em; (void)esz; (void)user;

	fj_kv_str(b, "native_arch", native_arch);
	fj_kv_str(b, "window_mode", win_mode);
	fj_begin_arr(b, "backends");
	for (int i = 0; BACKENDS[i].name; i++)
		fj_arr_str(b, BACKENDS[i].name);
	fj_end_arr(b);
	fj_kv_str(b, "translator_env", "FOS_TRANSLATOR");
	return 0;
}

/* app.run {arch,path,args} —— 决策路径并拉起 */
static int cmd_app_run(const fos_jobj *req, fos_jbuf *b,
		       char *em, size_t esz, void *user)
{
	(void)user;
	char arch[32], path[256], args[256] = "";
	char line[512], *argvp[32];
	int bi, i = 0;
	pid_t pid;

	fos_json_get_str(req, "arch", "", arch, sizeof(arch));
	fos_json_get_str(req, "path", "", path, sizeof(path));
	fos_json_get_str(req, "args", "", args, sizeof(args));

	if (!path[0]) {
		snprintf(em, esz, "缺少 path");
		return -32602;
	}
	/* 同架构 → native；异架构 → 选翻译后端 */
	if (!arch[0] || strcmp(arch, native_arch) == 0) {
		for (bi = 0; BACKENDS[bi].name; bi++)
			if (strcmp(BACKENDS[bi].name, "native") == 0)
				break;
		if (!BACKENDS[bi].name) {
			snprintf(em, esz, "internal: native 后端缺失");
			return -32603;
		}
	} else {
		bi = pick_backend(arch);
		if (bi < 0) {
			snprintf(em, esz, "架构 %s 无可用后端", arch);
			return -32602;
		}
	}

	snprintf(line, sizeof(line), "%s", BACKENDS[bi].cmd);
	{
		char *ph = strstr(line, "{PKG_PATH}");

		if (!ph) {
			snprintf(em, esz, "后端模板缺少 {PKG_PATH}");
			return -32603;
		}
		snprintf(ph, sizeof(line) - (size_t)(ph - line), "%s %s",
			 path, args);
	}

	/* 拆词 exec（参考实现不支持引号嵌套，真实系统交给 shell wrapper） */
	{
		char *tok = strtok(line, " ");

		while (tok && i < 31) {
			argvp[i++] = tok;
			tok = strtok(NULL, " ");
		}
		argvp[i] = NULL;
	}

	if (i == 0) {
		snprintf(em, esz, "空命令");
		return -32602;
	}
	if (posix_spawn(&pid, argvp[0], NULL, NULL, argvp, environ) != 0) {
		snprintf(em, esz, "拉起失败: %s", argvp[0]);
		return -32000;
	}
	int slot = -1;

	for (int c = 0; c < MAX_APPS; c++) {
		if (apps[c].used)
			continue;
		slot = c;
		break;
	}
	if (slot >= 0) {
		apps[slot].used = 1;
		apps[slot].pid = pid;
		snprintf(apps[slot].mode, sizeof(apps[slot].mode), "%s",
			 strcmp(BACKENDS[bi].name, "native") == 0 ?
			 "native" : BACKENDS[bi].name);
		snprintf(apps[slot].pkg, sizeof(apps[slot].pkg), "%s", path);
	}
	fj_kv_i64(b, "pid", pid);
	fj_kv_str(b, "via", BACKENDS[bi].name);
	return 0;
}

/* mode.set {mode:window|fullscreen} —— 形态切换（电脑并行/手机平板全屏） */
static int cmd_mode_set(const fos_jobj *req, fos_jbuf *b,
			char *em, size_t esz, void *user)
{
	(void)b; (void)user;
	char mode[16];

	fos_json_get_str(req, "mode", "", mode, sizeof(mode));
	if (strcmp(mode, "window") != 0 && strcmp(mode, "fullscreen") != 0) {
		snprintf(em, esz, "mode 只支持 window|fullscreen");
		return -32602;
	}
	snprintf(win_mode, sizeof(win_mode), "%s", mode);
	return 0;
}

/* apps.list —— 兼容层托管中的进程 */
static int cmd_apps_list(const fos_jobj *req, fos_jbuf *b,
			 char *em, size_t esz, void *user)
{
	(void)req; (void)em; (void)esz; (void)user;

	fj_begin_arr(b, "apps");
	for (int i = 0; i < MAX_APPS; i++) {
		int alive;

		if (!apps[i].used)
			continue;
		alive = (waitpid(apps[i].pid, NULL, WNOHANG) == 0);
		if (!alive) {
			apps[i].used = 0;
			continue;
		}
		fj_arr_obj(b);
		fj_kv_i64(b, "pid", apps[i].pid);
		fj_kv_str(b, "pkg", apps[i].pkg);
		fj_kv_str(b, "via", apps[i].mode);
		fj_end_obj(b);
	}
	fj_end_arr(b);
	return 0;
}

static int init_compat(void *user)
{
	(void)user;
	detect_arch();
	LOGI("virt-compat", "native=%s mode=%s", native_arch, win_mode);
	return 0;
}

static const fos_cmd_table TABLE[] = {
	{ "info",      cmd_info,      "运行环境与可用后端" },
	{ "app.run",   cmd_app_run,   "拉起异/同架构应用" },
	{ "mode.set",  cmd_mode_set,  "window|fullscreen 形态切换" },
	{ "apps.list", cmd_apps_list, "托管中进程列表" },
	{ NULL, NULL, NULL }
};

int main(int argc, char **argv)
{
	return fos_daemon_main(argc, argv, "virt-compat", TABLE,
			       init_compat, NULL);
}
