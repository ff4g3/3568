/*
 * 01_uart_protocol.c —— 串口 + 自定义帧协议 + 往返时延统计
 *
 * 对应 "RK3568 <-> STM32" 控制链路：RK3568 发关节目标，STM32 回 ACK/状态。
 *
 * 帧格式（小端）：
 *   +------+------+-------+-------+-----------+-------+
 *   | 0xAA | 0x55 | len   | seq   | payload   | crc16 |
 *   |  1B  |  1B  | 2B LE | 2B LE | len 字节  | 2B LE |
 *   +------+------+-------+-------+-----------+-------+
 *   CRC16-CCITT(0x1021, init=0xFFFF)，覆盖 len/seq/payload
 *
 * 知识点：
 *   1. termios raw 模式（cfmakeraw），关回显/换行转换
 *   2. 非阻塞 fd + poll() + 超时，避免 read/write 卡死
 *   3. 字节流没有边界，必须靠状态机 + 长度 + CRC 拼帧
 *   4. clock_gettime(CLOCK_MONOTONIC) 测时延（不要用 gettimeofday）
 *
 * 用法：
 *   ./01_uart_protocol --selftest 500
 *   ./01_uart_protocol -d /dev/ttyS3 -b 115200 send hello
 *   ./01_uart_protocol -d /dev/ttyS3 -b 115200 recv 10
 *
 * 注意：调试串口通常是 /dev/ttyS2 或 /dev/ttyFIQ0，别占用！
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <time.h>

#define HEAD0		0xAA
#define HEAD1		0x55
#define MAX_PAYLOAD	1024
#define FRAME_MAX	(MAX_PAYLOAD + 8)

struct parser {
	int state;
	uint16_t len;
	uint16_t seq;
	uint16_t idx;
	uint8_t payload[MAX_PAYLOAD];
	uint8_t crc_lo;
};

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFF;
	size_t i;
	int b;

	for (i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
					     : (uint16_t)(crc << 1);
	}
	return crc;
}

static size_t frame_build(uint8_t *out, uint16_t seq, const uint8_t *payload,
			  uint16_t len)
{
	uint16_t crc;

	out[0] = HEAD0;
	out[1] = HEAD1;
	out[2] = len & 0xff;
	out[3] = len >> 8;
	out[4] = seq & 0xff;
	out[5] = seq >> 8;
	memcpy(out + 6, payload, len);
	crc = crc16_ccitt(out + 2, (size_t)len + 4);
	out[6 + len] = crc & 0xff;
	out[7 + len] = crc >> 8;
	return (size_t)len + 8;
}

/* 返回 1 收齐一帧；0 继续；-1 CRC 错 */
static int parser_feed(struct parser *p, uint8_t b)
{
	uint16_t crc;

	switch (p->state) {
	case 0:
		if (b == HEAD0)
			p->state = 1;
		break;
	case 1:
		p->state = (b == HEAD1) ? 2 : (b == HEAD0 ? 1 : 0);
		break;
	case 2:
		p->len = b;
		p->state = 3;
		break;
	case 3:
		p->len |= (uint16_t)b << 8;
		if (p->len > MAX_PAYLOAD) {
			p->state = 0;
			break;
		}
		p->state = 4;
		break;
	case 4:
		p->seq = b;
		p->state = 5;
		break;
	case 5:
		p->seq |= (uint16_t)b << 8;
		p->idx = 0;
		p->state = (p->len == 0) ? 7 : 6;
		break;
	case 6:
		p->payload[p->idx++] = b;
		if (p->idx >= p->len)
			p->state = 7;
		break;
	case 7:
		p->crc_lo = b;
		p->state = 8;
		break;
	case 8: {
		uint8_t tmp[4 + MAX_PAYLOAD];

		tmp[0] = p->len & 0xff;
		tmp[1] = p->len >> 8;
		tmp[2] = p->seq & 0xff;
		tmp[3] = p->seq >> 8;
		memcpy(tmp + 4, p->payload, p->len);
		crc = crc16_ccitt(tmp, (size_t)p->len + 4);
		p->state = 0;
		if (crc != ((uint16_t)p->crc_lo | ((uint16_t)b << 8)))
			return -1;
		return 1;
	}
	default:
		p->state = 0;
		break;
	}
	return 0;
}

static int write_all(int fd, const uint8_t *buf, size_t len, int timeout_ms)
{
	size_t off = 0;

	while (off < len) {
		struct pollfd pfd = { .fd = fd, .events = POLLOUT };
		ssize_t n;
		int ret = poll(&pfd, 1, timeout_ms);

		if (ret <= 0)
			return -1;
		n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static speed_t baud_to_speed(int baud)
{
	switch (baud) {
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 921600: return B921600;
	case 1500000: return B1500000;
	default: return 0;
	}
}

static int serial_open(const char *dev, int baud)
{
	struct termios tio;
	speed_t sp = baud_to_speed(baud);
	int fd;

	if (!sp) {
		fprintf(stderr, "不支持的波特率: %d\n", baud);
		return -1;
	}

	fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return -1;
	}
	if (tcgetattr(fd, &tio) < 0) {
		fprintf(stderr, "tcgetattr: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	cfmakeraw(&tio);		/* 关回显、行缓冲、CR/LF 转换 */
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cflag &= ~CRTSCTS;	/* 无硬件流控；需要时改成 |= CRTSCTS */
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	cfsetispeed(&tio, sp);
	cfsetospeed(&tio, sp);

	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	tcflush(fd, TCIOFLUSH);
	return fd;
}

static int reader_loop(int fd, int seconds, int quiet)
{
	struct parser p;
	uint64_t deadline = now_us() + (uint64_t)seconds * 1000000ULL;

	memset(&p, 0, sizeof(p));
	while (now_us() < deadline) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		uint8_t buf[256];
		ssize_t n, i;
		int ret = poll(&pfd, 1, 200);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			return -1;
		}
		if (ret == 0)
			continue;

		n = read(fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			perror("read");
			return -1;
		}
		for (i = 0; i < n; i++) {
			int r = parser_feed(&p, buf[i]);

			if (r < 0 && !quiet)
				fprintf(stderr, "CRC 错误，已丢弃该帧\n");
			if (r == 1 && !quiet)
				printf("RX seq=%u len=%u payload=\"%.*s\"\n",
				       p.seq, p.len, (int)p.len, p.payload);
		}
	}
	return 0;
}

/* 用 pty 模拟"对端"，测协议解析和单程时延，不需要真实串口 */
static int selftest(int count)
{
	uint64_t *lat = calloc((size_t)count, sizeof(*lat));
	struct parser p;
	struct termios tio;
	int master, slave, i, n = 0;
	char *name;

	if (!lat || count <= 0)
		return -1;

	master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
		perror("posix_openpt");
		free(lat);
		return -1;
	}
	name = ptsname(master);
	if (!name) {
		free(lat);
		close(master);
		return -1;
	}
	slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (slave < 0) {
		perror("open pty slave");
		free(lat);
		close(master);
		return -1;
	}

	/* pty 默认会回显/转换换行，必须设成 raw，否则测的不是真实链路 */
	if (tcgetattr(slave, &tio) == 0) {
		cfmakeraw(&tio);
		tcsetattr(slave, TCSANOW, &tio);
	}

	printf("pty 自测: %s, %d 帧\n", name, count);
	for (i = 0; i < count; i++) {
		uint8_t frame[FRAME_MAX];
		char payload[32];
		uint64_t t0;
		size_t flen;
		int got = 0;

		snprintf(payload, sizeof(payload), "ping-%d", i);
		flen = frame_build(frame, (uint16_t)i, (uint8_t *)payload,
				   (uint16_t)strlen(payload));

		memset(&p, 0, sizeof(p));
		t0 = now_us();
		if (write_all(master, frame, flen, 1000) < 0) {
			perror("write pty");
			break;
		}

		while (!got && now_us() - t0 < 1000000ULL) {
			struct pollfd pfd = { .fd = slave, .events = POLLIN };
			uint8_t buf[64];
			ssize_t r, k;

			if (poll(&pfd, 1, 100) <= 0)
				continue;
			r = read(slave, buf, sizeof(buf));
			if (r <= 0)
				continue;
			for (k = 0; k < r && !got; k++) {
				int fr = parser_feed(&p, buf[k]);

				if (fr == 1) {
					if (p.seq != (uint16_t)i ||
					    p.len != strlen(payload) ||
					    memcmp(p.payload, payload, p.len))
						fprintf(stderr, "第 %d 帧内容不匹配\n", i);
					got = 1;
				}
			}
		}
		if (got)
			lat[n++] = now_us() - t0;
		else
			fprintf(stderr, "第 %d 帧超时\n", i);
	}

	if (n > 0) {
		uint64_t sum = 0;

		qsort(lat, (size_t)n, sizeof(*lat), cmp_u64);
		for (i = 0; i < n; i++)
			sum += lat[i];
		printf("成功 %d/%d 帧, 时延 us: min=%llu avg=%llu p50=%llu p99=%llu max=%llu\n",
		       n, count, (unsigned long long)lat[0],
		       (unsigned long long)(sum / (uint64_t)n),
		       (unsigned long long)lat[n * 50 / 100],
		       (unsigned long long)lat[n * 99 / 100],
		       (unsigned long long)lat[n - 1]);
	}

	free(lat);
	close(slave);
	close(master);
	return n == count ? 0 : -1;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"用法:\n"
		"  %s --selftest [帧数]                 # pty 自测，无需硬件\n"
		"  %s -d <串口> [-b 波特率] send <字符串>\n"
		"  %s -d <串口> [-b 波特率] recv [秒数]\n"
		"例子:\n"
		"  %s -d /dev/ttyS3 -b 115200 send hello\n",
		prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
	const char *dev = NULL;
	int baud = 115200;
	int i = 1;

	if (argc > 1 && !strcmp(argv[1], "--selftest"))
		return selftest(argc > 2 ? atoi(argv[2]) : 500) ? 1 : 0;

	while (i < argc) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "-b") && i + 1 < argc)
			baud = atoi(argv[++i]);
		else
			break;
		i++;
	}

	if (!dev || i >= argc) {
		usage(argv[0]);
		return 1;
	}

	if (!strcmp(argv[i], "send") && i + 1 < argc) {
		uint8_t frame[FRAME_MAX];
		size_t len = frame_build(frame, 1, (uint8_t *)argv[i + 1],
					 (uint16_t)strlen(argv[i + 1]));
		int fd = serial_open(dev, baud), k;

		if (fd < 0)
			return 1;
		if (write_all(fd, frame, len, 1000) < 0) {
			perror("write");
			close(fd);
			return 1;
		}
		printf("TX %zu 字节:", len);
		for (k = 0; k < (int)len; k++)
			printf(" %02x", frame[k]);
		printf("\n");
		close(fd);
		return 0;
	}

	if (!strcmp(argv[i], "recv")) {
		int seconds = (i + 1 < argc) ? atoi(argv[i + 1]) : 10;
		int fd = serial_open(dev, baud);

		if (fd < 0)
			return 1;
		printf("监听 %s，%d 秒...\n", dev, seconds);
		reader_loop(fd, seconds, 0);
		close(fd);
		return 0;
	}

	usage(argv[0]);
	return 1;
}
