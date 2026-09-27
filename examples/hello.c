/* SPDX-License-Identifier: MulanPSL-2.0 */
/*
 * hello.c — 示例应用的原生入口（供想编译真二进制的社区/学生替换 shell 版）
 *
 * 交叉编译（两架构）：
 *   aarch64-linux-gnu-gcc -static hello.c -o hello-app/bin/arm64/hello
 *   gcc -static hello.c -o hello-app/bin/x86_64/hello
 * 无交叉链时用本机 gcc 只打 x86_64，并把 manifest.json 的 arch 删到 ["x86_64"]。
 * 静态链接（-static）保证在 strict 沙箱 rootfs 里可直接运行。
 */
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	printf("Hello from .fos native binary!\n");
	for (int i = 1; i <= 3; i++) {
		printf("tick %d/3\n", i);
		sleep(1);
	}
	return 0;
}
