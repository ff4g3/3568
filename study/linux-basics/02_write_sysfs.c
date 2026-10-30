/*
 * 02_write_sysfs.c —— 通过 /sys 控制一个 LED
 *
 * 知识点：
 *   1. sysfs 不仅能读，很多节点还能写（写字符串，内核负责解析）
 *   2. sysfs 写入要"一次写完"，不要分多次 write，也不要依赖文件偏移
 *   3. O_WRONLY + write() 就够了，不需要 fseek
 *   4. 根文件系统常常只读，写 /sys 一般需要 root
 *
 * 用法：
 *   ./02_write_sysfs led1 1      # 点亮 /sys/class/leds/led1/brightness
 *   ./02_write_sysfs led1 0      # 熄灭
 *   ./02_write_sysfs led1 128    # 设一半亮度（如果驱动支持）
 *   ./02_write_sysfs --list      # 列出板子上所有 LED
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

static int list_leds(void)
{
	DIR *dir;
	struct dirent *de;

	dir = opendir("/sys/class/leds");
	if (!dir) {
		fprintf(stderr, "opendir(/sys/class/leds): %s\n", strerror(errno));
		return -1;
	}

	puts("板子上的 LED：");
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		printf("  %s\n", de->d_name);
	}
	closedir(dir);
	return 0;
}

int main(int argc, char *argv[])
{
	char path[256];
	char buf[32];
	int fd, len, value;

	if (argc == 2 && !strcmp(argv[1], "--list"))
		return list_leds();

	if (argc != 3) {
		fprintf(stderr,
			"用法: %s <led名字> <亮度值>\n"
			"      %s --list\n"
			"例子: %s led1 1\n", argv[0], argv[0], argv[0]);
		return 1;
	}

	value = atoi(argv[2]);
	snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", argv[1]);

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		if (errno == ENOENT)
			fprintf(stderr, "提示: 用 --list 看看 LED 名字是否写对\n");
		if (errno == EACCES)
			fprintf(stderr, "提示: 需要 root 权限\n");
		if (errno == EROFS)
			fprintf(stderr, "提示: 根文件系统只读，但 /sys 一般是可写的\n");
		return 1;
	}

	/*
	 * 只写一次。sysfs 的 write 会被内核当成一条完整命令处理，
	 * 分成多次 write 可能报 -EINVAL。
	 */
	len = snprintf(buf, sizeof(buf), "%d", value);
	if (write(fd, buf, len) != len) {
		fprintf(stderr, "write %s: %s\n", path, strerror(errno));
		close(fd);
		return 1;
	}

	close(fd);
	printf("已写入 %s = %d\n", path, value);

	/* 回读验证：把刚写的值读出来 */
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		memset(buf, 0, sizeof(buf));
		len = read(fd, buf, sizeof(buf) - 1);
		if (len > 0)
			printf("回读结果: %s", buf);
		close(fd);
	}
	return 0;
}
