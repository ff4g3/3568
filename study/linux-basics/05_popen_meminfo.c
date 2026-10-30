/*
 * 05_popen_meminfo.c —— system() / popen() 与"标准 IO"
 *
 * 知识点：
 *   1. system()：执行一条 shell 命令，等它结束，只拿退出码（看不到输出）
 *   2. popen()：执行命令并把它的 stdout 接到一个 FILE* 上，可以 fgets 读取
 *   3. pclose() 的返回值是 waitpid 状态，用 WEXITSTATUS 取真正退出码
 *   4. sscanf 从一行文本里提取数字
 *
 * 用法：
 *   ./05_popen_meminfo
 *
 * 说明：in_external/rk_pcba_test/echo_ddr_test.c:38 就是同款写法，
 *       用于读 /proc/zoneinfo 计算内存大小。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

static void system_demo(void)
{
	int ret;

	puts("---- system() 演示：只关心退出码 ----");
	ret = system("test -r /proc/meminfo");
	if (ret == -1) {
		perror("system");
		return;
	}
	if (WIFEXITED(ret))
		printf("命令退出码 = %d\n", WEXITSTATUS(ret));
}

static void popen_demo(void)
{
	FILE *fp;
	char line[256];
	unsigned long mem_total = 0, mem_avail = 0;
	int status;

	puts("\n---- popen() 演示：读取命令输出 ----");

	fp = popen("cat /proc/meminfo", "r");
	if (!fp) {
		fprintf(stderr, "popen: %s\n", strerror(errno));
		return;
	}

	while (fgets(line, sizeof(line), fp)) {
		unsigned long kb;

		if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) {
			mem_total = kb;
			printf("MemTotal      : %lu kB (%.1f MB)\n",
			       kb, kb / 1024.0);
		} else if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1) {
			mem_avail = kb;
			printf("MemAvailable  : %lu kB (%.1f MB)\n",
			       kb, kb / 1024.0);
		}
	}

	status = pclose(fp);
	if (status == -1) {
		perror("pclose");
		return;
	}
	if (WIFEXITED(status))
		printf("命令退出码 = %d\n", WEXITSTATUS(status));

	if (mem_total)
		printf("内存使用率    : %.1f%%\n",
		       100.0 * (mem_total - mem_avail) / mem_total);
}

int main(void)
{
	system_demo();
	popen_demo();

	puts("\n提示：popen 会 fork + exec /bin/sh，开销比直接 open/read 大；"
	     "\n      追求性能和安全的场景应直接用系统调用（见 01_read_sysfs.c）。");
	return 0;
}
