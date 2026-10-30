/*
 * 06_rga_scale.c —— 用 RGA(im2d API) 做硬件缩放/格式转换
 *
 * 对应你项目里的 "硬件加速缩放"。RGA 是 2D 加速器，缩放几乎不占 CPU。
 *
 * 知识点：
 *   1. RGA 用户态接口在 external/linux-rga（im2d API），链接 -lrga
 *   2. 流程：malloc 一块内存 -> importbuffer_virtualaddr -> wrapbuffer_handle
 *            -> imcheck -> imresize -> releasebuffer_handle
 *   3. 图像格式用 RK_FORMAT_*，NV12 = RK_FORMAT_YCbCr_420_SP
 *   4. RGA 对宽高/stride 有对齐要求（常见 16 字节），本例默认按对齐尺寸运行
 *   5. 生产环境不要用 malloc，而要用 dma-buf/DRM 内存，才能和 V4L2/MPP 零拷贝
 *
 * 用法：
 *   ./06_rga_scale -i in.nv12 -w 1280 -h 720 -W 640 -H 360 -o out.nv12 -n 100
 *   ./06_rga_scale -w 1280 -h 720 -W 640 -H 360      # 不读文件，生成测试图
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

#include "im2d.h"
#include "rga.h"

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

static void fill_nv12(uint8_t *buf, int w, int h, int seed)
{
	int x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			buf[y * w + x] = (uint8_t)((x + y + seed) & 0xff);
	/* UV 平面填中性灰，避免颜色异常 */
	memset(buf + w * h, 128, (size_t)w * h / 2);
}

static int read_file(const char *path, uint8_t *buf, size_t len)
{
	FILE *fp = fopen(path, "rb");
	size_t n;

	if (!fp) {
		fprintf(stderr, "fopen %s: %s\n", path, strerror(errno));
		return -1;
	}
	n = fread(buf, 1, len, fp);
	fclose(fp);
	if (n != len) {
		fprintf(stderr, "%s 只有 %zu 字节，期望 %zu\n", path, n, len);
		return -1;
	}
	return 0;
}

static int write_file(const char *path, const void *buf, size_t len)
{
	FILE *fp = fopen(path, "wb");
	size_t n;

	if (!fp) {
		fprintf(stderr, "fopen %s: %s\n", path, strerror(errno));
		return -1;
	}
	n = fwrite(buf, 1, len, fp);
	fclose(fp);
	return n == len ? 0 : -1;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"用法: %s [-i in.nv12] -w 1280 -h 720 -W 640 -H 360 "
		"[-o out.nv12] [-n 100]\n", p);
}

int main(int argc, char *argv[])
{
	const char *in_path = NULL, *out_path = NULL;
	int sw = 1280, sh = 720, dw = 640, dh = 360, iters = 100;
	size_t src_size, dst_size;
	uint8_t *src_buf = NULL, *dst_buf = NULL;
	rga_buffer_t src, dst;
	rga_buffer_handle_t src_handle = 0, dst_handle = 0;
	uint64_t *lat = NULL;
	double sum = 0;
	int i, ret = 1;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-i") && i + 1 < argc)
			in_path = argv[++i];
		else if (!strcmp(argv[i], "-o") && i + 1 < argc)
			out_path = argv[++i];
		else if (!strcmp(argv[i], "-w") && i + 1 < argc)
			sw = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-h") && i + 1 < argc)
			sh = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-W") && i + 1 < argc)
			dw = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-H") && i + 1 < argc)
			dh = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			iters = atoi(argv[++i]);
		else {
			usage(argv[0]);
			return 1;
		}
	}

	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || iters <= 0 ||
	    (sw & 15) || (sh & 15) || (dw & 15) || (dh & 15)) {
		fprintf(stderr, "宽高必须是 16 的整数倍（RGA 对齐要求）\n");
		return 1;
	}

	/* NV12: Y 平面 w*h，UV 平面 w*h/2 */
	src_size = (size_t)sw * sh * 3 / 2;
	dst_size = (size_t)dw * dh * 3 / 2;
	src_buf = malloc(src_size);
	dst_buf = malloc(dst_size);
	lat = calloc((size_t)iters, sizeof(*lat));
	if (!src_buf || !dst_buf || !lat) {
		fprintf(stderr, "malloc 失败\n");
		goto out;
	}

	if (in_path && read_file(in_path, src_buf, src_size) < 0)
		goto out;
	if (!in_path)
		fill_nv12(src_buf, sw, sh, 0);
	memset(dst_buf, 0, dst_size);

	/* 把用户态内存交给 RGA（C 接口用 im_handle_param_t 描述尺寸/格式） */
	{
		im_handle_param_t src_param = { (uint32_t)sw, (uint32_t)sh,
						RK_FORMAT_YCbCr_420_SP };
		im_handle_param_t dst_param = { (uint32_t)dw, (uint32_t)dh,
						RK_FORMAT_YCbCr_420_SP };

		src_handle = importbuffer_virtualaddr(src_buf, &src_param);
		dst_handle = importbuffer_virtualaddr(dst_buf, &dst_param);
	}
	if (!src_handle || !dst_handle) {
		fprintf(stderr, "importbuffer_virtualaddr 失败\n");
		goto release;
	}

	src = wrapbuffer_handle(src_handle, sw, sh, RK_FORMAT_YCbCr_420_SP);
	dst = wrapbuffer_handle(dst_handle, dw, dh, RK_FORMAT_YCbCr_420_SP);

	IM_STATUS check;

	check = imcheck(src, dst, (im_rect){0}, (im_rect){0});
	if (check != IM_STATUS_NOERROR) {
		fprintf(stderr, "imcheck 失败: %s\n", imStrError(check));
		goto release;
	}

	printf("RGA 缩放 %dx%d -> %dx%d，%d 次\n", sw, sh, dw, dh, iters);
	for (i = 0; i < iters; i++) {
		uint64_t t0 = now_us();
		IM_STATUS st = imresize(src, dst);

		lat[i] = now_us() - t0;
		if (st != IM_STATUS_SUCCESS) {
			fprintf(stderr, "imresize 失败: %s\n", imStrError(st));
			goto release;
		}
	}

	qsort(lat, (size_t)iters, sizeof(*lat), cmp_u64);
	for (i = 0; i < iters; i++)
		sum += (double)lat[i];
	printf("单次耗时(us): min=%llu avg=%.1f p50=%llu p99=%llu max=%llu\n",
	       (unsigned long long)lat[0], sum / iters,
	       (unsigned long long)lat[iters * 50 / 100],
	       (unsigned long long)lat[iters * 99 / 100],
	       (unsigned long long)lat[iters - 1]);

	if (out_path && write_file(out_path, dst_buf, dst_size) == 0)
		printf("结果已写入 %s\n", out_path);
	ret = 0;

release:
	if (src_handle)
		releasebuffer_handle(src_handle);
	if (dst_handle)
		releasebuffer_handle(dst_handle);
out:
	free(src_buf);
	free(dst_buf);
	free(lat);
	return ret;
}
