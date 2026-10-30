/*
 * 04_v4l2_capture.c —— V4L2 采集（mmap 流式 IO），保存原始帧并测帧率
 *
 * 这是视觉节点的第一步：从 /dev/videoX 拿 NV12/YUYV/MJPEG 帧。
 * 对应你项目里的 "V4L2 采集"。
 *
 * 知识点：
 *   1. V4L2 的固定流程：
 *        open -> QUERYCAP -> S_FMT -> REQBUFS -> QUERYBUF/mmap -> QBUF
 *             -> STREAMON -> poll -> DQBUF -> 处理 -> QBUF -> ... -> STREAMOFF
 *   2. mmap 把内核里的采集缓冲区映射到用户态，读出不需要额外拷贝
 *   3. buf.index 是缓冲区编号，不是帧号；帧时间戳看 buf.timestamp
 *   4. 支持格式取决于摄像头：USB UVC 常见 YUYV/MJPEG；Rockchip ISP 常见 NV12
 *
 * 用法：
 *   ./04_v4l2_capture --list
 *   ./04_v4l2_capture --list /dev/video1
 *   ./04_v4l2_capture -d /dev/video0 -W 1280 -H 720 -f NV12 -n 30 -o /tmp/cap
 *
 * 注意：MIPI 摄像头通常要先配置 media pipeline（media-ctl/rkaiq），
 *      直接打开 /dev/videoX 可能没有数据；先用 USB 摄像头跑通本例子。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/videodev2.h>

#define BUF_COUNT	4

struct buffer {
	void *start;
	size_t length;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, req, arg);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static const char *fourcc_str(uint32_t pix, char *buf)
{
	buf[0] = (char)(pix & 0xff);
	buf[1] = (char)((pix >> 8) & 0xff);
	buf[2] = (char)((pix >> 16) & 0xff);
	buf[3] = (char)((pix >> 24) & 0xff);
	buf[4] = '\0';
	return buf;
}

static uint32_t parse_fourcc(const char *s)
{
	if (!strcasecmp(s, "NV12"))
		return V4L2_PIX_FMT_NV12;
	if (!strcasecmp(s, "YUYV"))
		return V4L2_PIX_FMT_YUYV;
	if (!strcasecmp(s, "MJPEG") || !strcasecmp(s, "MJPG"))
		return V4L2_PIX_FMT_MJPEG;
	if (!strcasecmp(s, "RGB24"))
		return V4L2_PIX_FMT_RGB24;
	return v4l2_fourcc(s[0], s[1], s[2], s[3]);
}

static void list_formats(const char *dev)
{
	struct v4l2_fmtdesc fmt;
	struct v4l2_frmsizeenum fsize;
	int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return;
	}

	printf("设备 %s 支持的格式：\n", dev);
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	while (xioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
		char cc[8];

		printf("  [%u] %s (%s)", fmt.index, fourcc_str(fmt.pixelformat, cc),
		       fmt.description);
		memset(&fsize, 0, sizeof(fsize));
		fsize.pixel_format = fmt.pixelformat;
		while (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsize) == 0) {
			if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
				printf(" %ux%u", fsize.discrete.width,
				       fsize.discrete.height);
			else
				printf(" %ux%u-%ux%u", fsize.stepwise.min_width,
				       fsize.stepwise.min_height,
				       fsize.stepwise.max_width,
				       fsize.stepwise.max_height);
			fsize.index++;
		}
		printf("\n");
		fmt.index++;
	}
	close(fd);
}

static int set_format(int fd, uint32_t *pix, uint32_t *w, uint32_t *h,
		      uint32_t *bytesperline, uint32_t *sizeimage)
{
	struct v4l2_format f;

	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = *w;
	f.fmt.pix.height = *h;
	f.fmt.pix.pixelformat = *pix;
	f.fmt.pix.field = V4L2_FIELD_NONE;

	if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) {
		fprintf(stderr, "VIDIOC_S_FMT: %s\n", strerror(errno));
		return -1;
	}
	/* 驱动可能调整分辨率/格式，以返回值为准 */
	*pix = f.fmt.pix.pixelformat;
	*w = f.fmt.pix.width;
	*h = f.fmt.pix.height;
	*bytesperline = f.fmt.pix.bytesperline;
	*sizeimage = f.fmt.pix.sizeimage;
	return 0;
}

static int req_buffers(int fd, struct buffer *bufs, uint32_t *count)
{
	struct v4l2_requestbuffers req;
	uint32_t i;

	memset(&req, 0, sizeof(req));
	req.count = *count;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		fprintf(stderr, "VIDIOC_REQBUFS: %s\n", strerror(errno));
		return -1;
	}
	if (req.count < 2) {
		fprintf(stderr, "缓冲区太少: %u\n", req.count);
		return -1;
	}

	for (i = 0; i < req.count; i++) {
		struct v4l2_buffer b;

		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
			fprintf(stderr, "VIDIOC_QUERYBUF: %s\n", strerror(errno));
			return -1;
		}
		bufs[i].length = b.length;
		bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, (off_t)b.m.offset);
		if (bufs[i].start == MAP_FAILED) {
			fprintf(stderr, "mmap: %s\n", strerror(errno));
			return -1;
		}
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
			fprintf(stderr, "VIDIOC_QBUF: %s\n", strerror(errno));
			return -1;
		}
	}
	*count = req.count;
	return 0;
}

static int save_blob(const char *dir, const char *name, const void *data, size_t len)
{
	char path[512];
	FILE *fp;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "fopen %s: %s\n", path, strerror(errno));
		return -1;
	}
	n = fwrite(data, 1, len, fp);
	fclose(fp);
	if (n != len) {
		fprintf(stderr, "写 %s 只写了 %zu/%zu\n", path, n, len);
		return -1;
	}
	printf("  -> %s (%zu 字节)\n", path, len);
	return 0;
}

/* 把 NV12 的 Y 平面存成 PGM（大多数看图软件都能打开） */
static int save_nv12_preview(const char *dir, const char *name,
			     const uint8_t *data, uint32_t w, uint32_t h,
			     uint32_t bpl)
{
	char path[512];
	FILE *fp;
	uint32_t y;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fp = fopen(path, "wb");
	if (!fp)
		return -1;
	fprintf(fp, "P5\n%u %u\n255\n", w, h);
	for (y = 0; y < h; y++)
		fwrite(data + (size_t)y * bpl, 1, w, fp);
	fclose(fp);
	printf("  -> %s (PGM 预览)\n", path);
	return 0;
}

static uint8_t clip_u8(int v)
{
	return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* YUYV -> PPM(P6)，方便在 PC 上直接看 */
static int save_yuyv_ppm(const char *dir, const char *name,
			 const uint8_t *data, uint32_t w, uint32_t h,
			 uint32_t bpl)
{
	char path[512];
	FILE *fp;
	uint8_t *rgb = malloc((size_t)w * h * 3);
	uint32_t x, y;

	if (!rgb)
		return -1;
	for (y = 0; y < h; y++) {
		const uint8_t *row = data + (size_t)y * bpl;

		for (x = 0; x + 1 < w; x += 2) {
			int y0 = row[0] - 16, u = row[1] - 128;
			int y1 = row[2] - 16, v = row[3] - 128;
			int c0 = 298 * y0, c1 = 298 * y1;
			uint8_t *p0 = rgb + ((size_t)y * w + x) * 3;
			uint8_t *p1 = p0 + 3;

			p0[0] = clip_u8((c0 + 409 * v + 128) >> 8);
			p0[1] = clip_u8((c0 - 100 * u - 208 * v + 128) >> 8);
			p0[2] = clip_u8((c0 + 516 * u + 128) >> 8);
			p1[0] = clip_u8((c1 + 409 * v + 128) >> 8);
			p1[1] = clip_u8((c1 - 100 * u - 208 * v + 128) >> 8);
			p1[2] = clip_u8((c1 + 516 * u + 128) >> 8);
			row += 4;
		}
	}

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fp = fopen(path, "wb");
	if (!fp) {
		free(rgb);
		return -1;
	}
	fprintf(fp, "P6\n%u %u\n255\n", w, h);
	fwrite(rgb, 1, (size_t)w * h * 3, fp);
	fclose(fp);
	free(rgb);
	printf("  -> %s (PPM 预览)\n", path);
	return 0;
}

static void save_frame(const char *dir, int idx, const uint8_t *data,
		       uint32_t pix, uint32_t w, uint32_t h, uint32_t bpl,
		       uint32_t size)
{
	char name[64];

	switch (pix) {
	case V4L2_PIX_FMT_NV12:
		snprintf(name, sizeof(name), "frame_%04d.nv12", idx);
		save_blob(dir, name, data, size);
		snprintf(name, sizeof(name), "frame_%04d.pgm", idx);
		save_nv12_preview(dir, name, data, w, h, bpl ? bpl : w);
		break;
	case V4L2_PIX_FMT_YUYV:
		snprintf(name, sizeof(name), "frame_%04d.yuyv", idx);
		save_blob(dir, name, data, size);
		snprintf(name, sizeof(name), "frame_%04d.ppm", idx);
		save_yuyv_ppm(dir, name, data, w, h, bpl ? bpl : w * 2);
		break;
	case V4L2_PIX_FMT_MJPEG:
		snprintf(name, sizeof(name), "frame_%04d.jpg", idx);
		save_blob(dir, name, data, size);
		break;
	case V4L2_PIX_FMT_RGB24:
		snprintf(name, sizeof(name), "frame_%04d.ppm", idx);
		{
			char path[512];
			FILE *fp;

			snprintf(path, sizeof(path), "%s/%s", dir, name);
			fp = fopen(path, "wb");
			if (fp) {
				fprintf(fp, "P6\n%u %u\n255\n", w, h);
				fwrite(data, 1, size, fp);
				fclose(fp);
				printf("  -> %s (PPM)\n", path);
			}
		}
		break;
	default:
		snprintf(name, sizeof(name), "frame_%04d.bin", idx);
		save_blob(dir, name, data, size);
		break;
	}
}

int main(int argc, char *argv[])
{
	const char *dev = "/dev/video0";
	const char *outdir = "/tmp/cap";
	uint32_t w = 1280, h = 720, pix = V4L2_PIX_FMT_NV12;
	uint32_t bpl = 0, size = 0, nbufs = BUF_COUNT;
	struct buffer bufs[BUF_COUNT];
	struct v4l2_capability cap;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int fd = -1, count = 10, captured = 0, timeouts = 0;
	uint64_t first_ts = 0, last_ts = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--list")) {
			if (i + 1 < argc && argv[i + 1][0] != '-')
				dev = argv[++i];
			list_formats(dev);
			return 0;
		} else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			dev = argv[++i];
		} else if (!strcmp(argv[i], "-W") && i + 1 < argc) {
			w = (uint32_t)atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-H") && i + 1 < argc) {
			h = (uint32_t)atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
			pix = parse_fourcc(argv[++i]);
		} else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			count = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
			outdir = argv[++i];
		} else {
			fprintf(stderr, "未知参数: %s\n"
				"用法: %s [-d /dev/video0] [-W 1280] [-H 720] "
				"[-f NV12|YUYV|MJPEG|RGB24] [-n 10] [-o /tmp/cap]\n"
				"      %s --list [设备]\n",
				argv[i], argv[0], argv[0]);
			return 1;
		}
	}
	if (count <= 0 || count > 1000) {
		fprintf(stderr, "帧数应在 1..1000\n");
		return 1;
	}

	fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return 1;
	}

	memset(&cap, 0, sizeof(cap));
	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		fprintf(stderr, "VIDIOC_QUERYCAP: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("设备: %s (%s), driver=%s\n", (char *)cap.card, dev,
	       (char *)cap.driver);

	if (set_format(fd, &pix, &w, &h, &bpl, &size) < 0) {
		close(fd);
		return 1;
	}

	{
		char cc[8];

		printf("采集格式: %s %ux%u, bytesperline=%u sizeimage=%u\n",
		       fourcc_str(pix, cc), w, h, bpl, size);
	}

	mkdir(outdir, 0755);
	memset(bufs, 0, sizeof(bufs));
	if (req_buffers(fd, bufs, &nbufs) < 0 || nbufs > BUF_COUNT)
		goto fail_close;
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		fprintf(stderr, "VIDIOC_STREAMON: %s\n", strerror(errno));
		goto fail_unmap;
	}

	printf("开始采集 %d 帧，保存到 %s\n", count, outdir);
	while (captured < count) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		struct v4l2_buffer b;
		uint64_t ts;
		int ret = poll(&pfd, 1, 2000);

		if (ret == 0) {
			if (++timeouts >= 3) {
				fprintf(stderr, "连续超时，摄像头没有出帧\n");
				break;
			}
			continue;
		}
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN)
				continue;
			fprintf(stderr, "VIDIOC_DQBUF: %s\n", strerror(errno));
			break;
		}

		ts = (uint64_t)b.timestamp.tv_sec * 1000000ULL +
		     (uint64_t)b.timestamp.tv_usec;
		if (!first_ts)
			first_ts = ts;
		last_ts = ts;

		printf("帧 %d: buf.index=%u bytesused=%u 时间戳=%llu.%06llu\n",
		       captured, b.index, b.bytesused,
		       (unsigned long long)(ts / 1000000ULL),
		       (unsigned long long)(ts % 1000000ULL));
		save_frame(outdir, captured, bufs[b.index].start, pix, w, h, bpl,
			   b.bytesused ? b.bytesused : size);

		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
			fprintf(stderr, "VIDIOC_QBUF: %s\n", strerror(errno));
			break;
		}
		captured++;
		timeouts = 0;
	}

	if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0)
		perror("VIDIOC_STREAMOFF");

	if (captured > 1 && last_ts > first_ts) {
		double seconds = (double)(last_ts - first_ts) / 1000000.0;

		printf("\n共 %d 帧，时间跨度 %.3f 秒，平均 %.1f fps\n",
		       captured, seconds, (captured - 1) / seconds);
	} else {
		printf("\n共 %d 帧\n", captured);
	}

fail_unmap:
	for (i = 0; i < (int)nbufs; i++) {
		if (bufs[i].start && bufs[i].start != MAP_FAILED)
			munmap(bufs[i].start, bufs[i].length);
	}
fail_close:
	close(fd);
	return captured > 0 ? 0 : 1;
}

