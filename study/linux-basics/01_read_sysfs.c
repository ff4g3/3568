/*
 * 01_read_sysfs.c —— 用最原始的 open/read/close 读取 /sys、/proc 里的文本
 *
 * 知识点：
 *   1. sysfs/procfs 是"内核暴露给用户态的文件"，内容都是文本
 *   2. open() -> read() -> close() 的完整流程
 *   3. read() 可能被信号打断(EINTR)，也可能一次读不完(循环读)
 *   4. 读到的字符串要自己补 '\0'，并去掉结尾换行
 *
 * 编译：见同目录 Makefile
 * 运行：./01_read_sysfs            # 读默认的几个节点
 *       ./01_read_sysfs <任意文件> # 读指定文件
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/*
 * 读一个文本文件到 buf（最多 len-1 字节，自动补 '\0'）。
 * 返回实际读到的字节数，失败返回 -1。
 */
static int read_text(const char *path, char *buf, size_t len)
{
	int fd;
	ssize_t n;
	size_t total = 0;

	if (!path || !buf || len == 0)
		return -1;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %-55s 失败: %s\n", path, strerror(errno));
		return -1;
	}

	while (total + 1 < len) {
		n = read(fd, buf + total, len - total - 1);
		if (n < 0) {
			if (errno == EINTR)	/* 被信号打断，重试 */
				continue;
			fprintf(stderr, "read %s 失败: %s\n", path, strerror(errno));
			close(fd);
			return -1;
		}
		if (n == 0)			/* 读到文件尾 */
			break;
		total += (size_t)n;
	}
	buf[total] = '\0';

	/* 去掉结尾的换行符，方便 printf */
	while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r'))
		buf[--total] = '\0';

	close(fd);
	return (int)total;
}

static void show_file(const char *path)
{
	char buf[512];
	int n = read_text(path, buf, sizeof(buf));

	if (n < 0) {
		printf("%-55s : <打不开>\n", path);
		return;
	}
	printf("%-55s : %s\n", path, buf);
}

int main(int argc, char *argv[])
{
	char buf[512];
	char path[256];
	int i, n;

	if (argc > 1) {
		for (i = 1; i < argc; i++)
			show_file(argv[i]);
		return 0;
	}

	puts("================ 板子基础信息 ================");
	show_file("/proc/device-tree/model");
	show_file("/proc/version");

	puts("\n================ CPU / 温度 ==================");
	show_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
	show_file("/sys/class/thermal/thermal_zone0/type");

	/* 温度单位是 0.001 摄氏度，需要自己做除法 */
	n = read_text("/sys/class/thermal/thermal_zone0/temp", buf, sizeof(buf));
	if (n > 0) {
		long milli = atol(buf);
		printf("%-55s : %ld (%.1f C)\n",
		       "/sys/class/thermal/thermal_zone0/temp",
		       milli, milli / 1000.0);
	} else {
		printf("%-55s : <读不到，可能没有这个节点>\n",
		       "/sys/class/thermal/thermal_zone0/temp");
	}

	puts("\n================ 内存 ========================");
	show_file("/proc/meminfo");

	puts("\n提示：可用 './01_read_sysfs 路径' 读任意节点，例如：");
	snprintf(path, sizeof(path), "/sys/class/leds");
	puts("  ./01_read_sysfs /sys/class/thermal/thermal_zone0/temp");
	puts("  cat /sys/kernel/debug/gpio   # 需要 debugfs（一般只有 root）");
	return 0;
}
