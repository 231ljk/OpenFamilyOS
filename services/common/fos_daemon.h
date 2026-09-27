/* SPDX-License-Identifier: MIT */
/*
 * fos_daemon.h — 守护进程通用 main 样板（header-only）
 *
 * 七个服务共用同一启动流程，把差异收敛到「命令表 + socket 名 +
 * 可选的启动前初始化 / 退出前清理」：
 *
 *   static const fos_cmd_table my_table[] = { ... {NULL,NULL,NULL} };
 *   int main(int argc, char **argv)
 *   { return fos_daemon_main(argc, argv, "super-ring", my_table, NULL, NULL); }
 *
 * 行为约定：
 *   --self-test   打印服务命令表后立即返回 0（CI 冒烟测试用）
 *   --sock PATH   覆盖 socket 路径（单元测试用，配合 FOS_RUN_DIR）
 *   -h / --help   用法
 *   默认阻塞运行 fos_ipc_serve，收到 SIGTERM/SIGINT 优雅退出。
 *
 * 信号处理只往 self-pipe 写一个字节（async-signal-safe），
 * 全部逻辑仍在主循环完成。
 */
#ifndef FOS_DAEMON_H
#define FOS_DAEMON_H

#include "fos_ipc.h"
#include "fos_log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*fos_daemon_init_fn)(void *user);
typedef void (*fos_daemon_fini_fn)(void *user);

/* self-pipe 写端：信号处理函数里唯一可写的 fd */
static volatile sig_atomic_t g_fos_sig_fd = -1;

static void fos_on_signal(int sig)
{
	char b = (char)sig;
	int fd = g_fos_sig_fd;

	if (fd >= 0) {
		ssize_t r = write(fd, &b, 1);

		(void)r;   /* 信号上下文里唯一允许的动作 */
	}
}

static int fos_daemon_main(int argc, char **argv, const char *name,
			   const fos_cmd_table *table,
			   fos_daemon_init_fn init, fos_daemon_fini_fn fini)
{
	char sock[512] = "";
	void *user = NULL;
	int pipefd[2] = { -1, -1 }, lfd, rc;
	const char *override = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--self-test") == 0) {
			printf("%s: self-test ok (cmds:\n", name);
			for (int c = 0; table[c].cmd; c++)
				printf("  %-18s %s\n", table[c].cmd,
				       table[c].help ? table[c].help : "");
			printf(")\n");
			return 0;
		}
		if (strcmp(argv[i], "--sock") == 0 && i + 1 < argc) {
			override = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0) {
			printf("用法: %s [--self-test] [--sock PATH]\n", name);
			printf("默认 socket: $FOS_RUN_DIR 或 /run/fos 下的 %s.sock\n",
			       name);
			return 0;
		}
		fprintf(stderr, "%s: 未知参数 %s\n", name, argv[i]);
		return 2;
	}

	if (override)
		snprintf(sock, sizeof(sock), "%s", override);
	else
		fos_ipc_sockpath(name, sock, sizeof(sock));

	if (init && init(user) != 0) {
		LOGE(name, "初始化失败");
		return 1;
	}

	lfd = fos_ipc_listen(sock, 0660);
	if (lfd < 0) {
		LOGE(name, "监听 %s 失败: %s", sock, strerror(errno));
		if (fini)
			fini(user);
		return 1;
	}

	if (pipe(pipefd) == 0) {
		g_fos_sig_fd = pipefd[1];
		fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
		signal(SIGTERM, fos_on_signal);
		signal(SIGINT, fos_on_signal);
		signal(SIGPIPE, SIG_IGN);
	} else {
		LOGW(name, "self-pipe 创建失败，信号处理退化为直接退出");
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
	}

	LOGI(name, "listening on %s (pid=%d)", sock, getpid());
	rc = fos_ipc_serve(lfd, table, user, pipefd[0]);
	LOGI(name, "shutdown (rc=%d)", rc);

	close(lfd);
	unlink(sock);
	if (pipefd[0] >= 0)
		close(pipefd[0]);
	if (pipefd[1] >= 0)
		close(pipefd[1]);
	if (fini)
		fini(user);
	return rc == 0 ? 0 : 1;
}

#endif /* FOS_DAEMON_H */
