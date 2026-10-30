/*
 * 03_read_input.c —— 读取 /dev/input/eventX 的按键事件
 *
 * 知识点：
 *   1. /dev/input/event* 是"字符设备"，read() 读到的是二进制 struct input_event
 *   2. 设备没有数据时会阻塞，所以配合 select() 实现超时
 *   3. struct input_event 里的 type/code/value 是 Linux 输入子系统的固定协议
 *   4. 非阻塞 fd + select 的经典组合
 *
 * 用法：
 *   ./03_read_input                 # 默认 /dev/input/event0，10 秒超时
 *   ./03_read_input /dev/input/event1 30
 *
 * 注意：先 cat /proc/bus/input/devices 找到按键对应的 event 号。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <linux/input.h>

int main(int argc, char *argv[])
{
	const char *dev = (argc > 1) ? argv[1] : "/dev/input/event0";
	int timeout_s = (argc > 2) ? atoi(argv[2]) : 10;
	int fd;

	fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		fprintf(stderr, "提示: cat /proc/bus/input/devices 查看有哪些 event\n");
		return 1;
	}

	printf("正在监听 %s，%d 秒内没有事件就退出...\n", dev, timeout_s);

	for (;;) {
		fd_set rfds;
		struct timeval tv;
		struct input_event ev;
		ssize_t n;
		int ret;

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		tv.tv_sec = timeout_s;
		tv.tv_usec = 0;

		ret = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (ret < 0) {
			if (errno == EINTR)	/* 被 Ctrl+C 等信号打断 */
				continue;
			perror("select");
			break;
		}
		if (ret == 0) {
			puts("超时，没有按键事件");
			break;
		}

		n = read(fd, &ev, sizeof(ev));
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			perror("read");
			break;
		}
		if (n == 0) {			/* 设备返回 EOF（例如 /dev/null） */
			puts("设备返回 EOF，退出");
			break;
		}
		if (n != (ssize_t)sizeof(ev))	/* 正常情况下不会发生 */
			continue;

		/* EV_SYN 只是分隔符，跳过让输出更干净 */
		if (ev.type == EV_SYN)
			continue;

		printf("[%ld.%06ld] type=%u code=%u value=%d",
		       (long)ev.time.tv_sec, (long)ev.time.tv_usec,
		       ev.type, ev.code, ev.value);

		if (ev.type == EV_KEY) {
			if (ev.value == 1)
				printf("  (按下 code=%u)", ev.code);
			else if (ev.value == 0)
				printf("  (松开 code=%u)", ev.code);
			else
				printf("  (长按重复 code=%u)", ev.code);
		}
		putchar('\n');
	}

	close(fd);
	return 0;
}
