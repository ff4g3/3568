/*
 * 08_dmabuf_probe.c —— 探测 DMA-BUF 能力（低拷贝流水线的钥匙）
 *
 * 目标：证明"摄像头缓冲区可以不 memcpy 就交给 RGA / MPP / 显示"。
 *
 * 知识点：
 *   1. V4L2 用 MMAP 申请缓冲区后，可以用 VIDIOC_EXPBUF 导出一个 dma-buf fd
 *   2. 这个 fd 能被别的驱动/库 import（RGA: importbuffer_fd；MPP/DRM 同理）
 *   3. mmap 同一个 dma-buf fd 只是建立映射，不会复制像素
 *   4. /dev/dma_heap/<name> 是用户态主动申请 dma-buf 的入口（内核 5.10+ 常见）
 *   5. 真正的零拷贝 = 全链路都传 fd，不经过 CPU 拷贝
 *
 * 用法：
 *   ./08_dmabuf_probe -d /dev/video0 -W 1280 -H 720
 *
 * 输出示例：
 *   V4L2 buf 0: size=1382400 expbuf_fd=5 mmap_ok
 *   dma_heap/system: fd=6 len=1382400 mmap_ok
 *   => 把 expbuf_fd 传给 RGA/MPP 即可避免一次 memcpy
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#if defined(__has_include)
# if __has_include(<linux/dma-heap.h>)
#  include <linux/dma-heap.h>
# endif
#endif

#ifndef DMA_HEAP_IOCTL_ALLOC
struct dma_heap_allocation_data {
	uint64_t len;
	uint32_t fd;
	uint32_t fd_flags;
	uint64_t heap_flags;
};
#define DMA_HEAP_IOC_MAGIC		'H'
#define DMA_HEAP_IOCTL_ALLOC		_IOWR(DMA_HEAP_IOC_MAGIC, 0x0, \
					      struct dma_heap_allocation_data)
#endif

static int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, req, arg);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static int probe_v4l2(const char *dev, int w, int h)
{
	struct v4l2_format f;
	struct v4l2_requestbuffers req;
	int fd, i, ret = 0;

	fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return -1;
	}

	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = (uint32_t)w;
	f.fmt.pix.height = (uint32_t)h;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	f.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) {
		fprintf(stderr, "VIDIOC_S_FMT: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	memset(&req, 0, sizeof(req));
	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		fprintf(stderr, "VIDIOC_REQBUFS: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	printf("V4L2 %s: %dx%d %c%c%c%c, %u 个缓冲区\n", dev,
	       f.fmt.pix.width, f.fmt.pix.height,
	       f.fmt.pix.pixelformat & 0xff,
	       (f.fmt.pix.pixelformat >> 8) & 0xff,
	       (f.fmt.pix.pixelformat >> 16) & 0xff,
	       (f.fmt.pix.pixelformat >> 24) & 0xff, req.count);

	for (i = 0; i < (int)req.count; i++) {
		struct v4l2_buffer b;
		struct v4l2_exportbuffer exp;
		void *p;

		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = (uint32_t)i;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
			fprintf(stderr, "VIDIOC_QUERYBUF: %s\n", strerror(errno));
			ret = -1;
			break;
		}

		memset(&exp, 0, sizeof(exp));
		exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		exp.index = (uint32_t)i;
		exp.flags = O_RDWR | O_CLOEXEC;
		if (xioctl(fd, VIDIOC_EXPBUF, &exp) < 0) {
			fprintf(stderr, "VIDIOC_EXPBUF(buf %d): %s "
				"（驱动可能不支持 dma-buf 导出）\n", i, strerror(errno));
			ret = -1;
			break;
		}

		/* mmap dma-buf fd：只建立映射，不复制数据 */
		p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, exp.fd, 0);
		printf("  V4L2 buf %d: size=%u expbuf_fd=%d mmap=%s\n", i, b.length,
		       exp.fd, p == MAP_FAILED ? "失败" : "ok");
		if (p != MAP_FAILED)
			munmap(p, b.length);
		close(exp.fd);
	}

	close(fd);
	return ret;
}

static int probe_heap(void)
{
	struct dma_heap_allocation_data data;
	void *p;
	int heap, ret = 0;

	heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	if (heap < 0) {
		printf("dma_heap/system 不存在（内核可能未开 CONFIG_DMABUF_HEAPS）\n");
		return 0;
	}

	memset(&data, 0, sizeof(data));
	data.len = 1280 * 720 * 3 / 2;
	data.fd_flags = O_RDWR | O_CLOEXEC;
	if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
		fprintf(stderr, "DMA_HEAP_IOCTL_ALLOC: %s\n", strerror(errno));
		close(heap);
		return -1;
	}
	close(heap);

	p = mmap(NULL, data.len, PROT_READ | PROT_WRITE, MAP_SHARED, (int)data.fd, 0);
	printf("dma_heap/system: fd=%u len=%llu mmap=%s\n", data.fd,
	       (unsigned long long)data.len, p == MAP_FAILED ? "失败" : "ok");
	if (p == MAP_FAILED)
		ret = -1;
	else
		munmap(p, data.len);
	close((int)data.fd);
	return ret;
}

int main(int argc, char *argv[])
{
	const char *dev = "/dev/video0";
	int w = 1280, h = 720, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "-W") && i + 1 < argc)
			w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-H") && i + 1 < argc)
			h = atoi(argv[++i]);
		else {
			fprintf(stderr, "用法: %s [-d /dev/video0] [-W 1280] [-H 720]\n",
				argv[0]);
			return 1;
		}
	}

	printf("=== 1. V4L2 缓冲区导出为 dma-buf ===\n");
	probe_v4l2(dev, w, h);
	printf("\n=== 2. 用户态 dma-heap 分配 ===\n");
	probe_heap();
	printf("\n结论: 把 expbuf_fd 传给 RGA(importbuffer_fd)/MPP/DRM，\n"
	       "      整条链路只传 fd，不需要 memcpy，这就是低拷贝流水线的基础。\n");
	return 0;
}
