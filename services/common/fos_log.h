/* SPDX-License-Identifier: MulanPSL-2.0 */
/*
 * fos_log.h — 守护进程统一日志（header-only，stderr + 可选文件）
 * 约定：systemd 服务一律输出 stderr，由 journald 收集；
 *       FOS_LOG_FILE 环境变量存在时额外落盘（开发调试用）。
 * 级别：FOS_LOG_LEVEL=0 DEBUG / 1 INFO（默认）/ 2 WARN / 3 ERROR
 */
#ifndef FOS_LOG_H
#define FOS_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *fos_log_level_name(int lvl)
{
	switch (lvl) {
	case 0: return "DEBUG";
	case 1: return "INFO";
	case 2: return "WARN";
	default: return "ERROR";
	}
}

static void fos_log(int lvl, const char *tag, const char *fmt, ...)
{
	static int inited;
	static int min_lvl = 1;
	va_list ap;
	char ts[40];
	const char *path;
	FILE *fp;

	if (!inited) {
		const char *e = getenv("FOS_LOG_LEVEL");

		min_lvl = (e && *e) ? atoi(e) : 1;
		inited = 1;
	}
	if (lvl < min_lvl)
		return;

	{
		time_t now = time(NULL);
		struct tm tm;

		localtime_r(&now, &tm);
		strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tm);
	}

	fprintf(stderr, "%s [%s] %s: ", ts, fos_log_level_name(lvl), tag);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);

	path = getenv("FOS_LOG_FILE");
	if (path && *path) {
		fp = fopen(path, "ae");
		if (fp) {
			fprintf(fp, "%s [%s] %s: ", ts,
				fos_log_level_name(lvl), tag);
			va_start(ap, fmt);
			vfprintf(fp, fmt, ap);
			va_end(ap);
			fputc('\n', fp);
			fclose(fp);
		}
	}
}

#define LOGD(tag, ...) fos_log(0, tag, __VA_ARGS__)
#define LOGI(tag, ...) fos_log(1, tag, __VA_ARGS__)
#define LOGW(tag, ...) fos_log(2, tag, __VA_ARGS__)
#define LOGE(tag, ...) fos_log(3, tag, __VA_ARGS__)

#endif /* FOS_LOG_H */
