/*
 * 05_fb_draw.c —— 用 /dev/fb0 直接画到屏幕（framebuffer + mmap）
 *
 * 对应 "屏幕" 这一路外设：很多 Rockchip 镜像仍开启 fbdev 模拟，会生成 /dev/fb0。
 * 如果板子上没有 /dev/fb0，说明只走了 DRM/KMS，可以先用 weston + kmssrc
 * （external/gstreamer-rockchip/gst/kmssrc），再进阶到直接写 DRM ioctl。
 *
 * 知识点：
 *   1. 字符设备也能 mmap：fb 的显存被映射进用户态，写内存就等于画屏
 *   2. FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO 拿到分辨率、bpp、行跨度
 *   3. 像素格式由 var.red/green/blue 的 offset/length 描述，不能硬编码 RGB 顺序
 *   4. 画完不需要"提交"（fbdev 不是双缓冲），但 tearing 取决于驱动
 *
 * 用法：
 *   ./05_fb_draw                 # /dev/fb0，画 60 帧彩条+移动方块
 *   ./05_fb_draw -d /dev/fb0 -n 0   # 只画一帧（0 表示不限制）
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
#include <linux/fb.h>

static uint32_t pack_color(const struct fb_var_screeninfo *var,
			   uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t v = 0;

	v |= ((uint32_t)r >> (8 - var->red.length)) << var->red.offset;
	v |= ((uint32_t)g >> (8 - var->green.length)) << var->green.offset;
	v |= ((uint32_t)b >> (8 - var->blue.length)) << var->blue.offset;
	if (var->transp.length)
		v |= ((uint32_t)0xff >> (8 - var->transp.length)) << var->transp.offset;
	return v;
}

static void put_pixel(uint8_t *fb, const struct fb_var_screeninfo *var,
		      const struct fb_fix_screeninfo *fix,
		      uint32_t x, uint32_t y, uint32_t color)
{
	uint8_t *p = fb + (size_t)y * fix->line_length +
		     (size_t)x * (var->bits_per_pixel / 8);

	switch (var->bits_per_pixel) {
	case 16:
		*(uint16_t *)p = (uint16_t)color;
		break;
	case 32:
		*(uint32_t *)p = color;
		break;
	default:
		break;	/* 24bpp 比较少见，先不处理 */
	}
}

static void fill_rect(uint8_t *fb, const struct fb_var_screeninfo *var,
		      const struct fb_fix_screeninfo *fix,
		      uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
		      uint32_t color)
{
	uint32_t x, y;

	for (y = y0; y < y0 + h && y < var->yres; y++)
		for (x = x0; x < x0 + w && x < var->xres; x++)
			put_pixel(fb, var, fix, x, y, color);
}

int main(int argc, char *argv[])
{
	const char *dev = "/dev/fb0";
	int frames = 60;
	int fps = 30;
	int fd, i, arg;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	uint8_t *fb;
	size_t map_len;
	const uint32_t colors[8] = {
		0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
		0xff00ff, 0xff0000, 0x0000ff, 0x000000
	};

	for (arg = 1; arg < argc; arg++) {
		if (!strcmp(argv[arg], "-d") && arg + 1 < argc)
			dev = argv[++arg];
		else if (!strcmp(argv[arg], "-n") && arg + 1 < argc)
			frames = atoi(argv[++arg]);
		else if (!strcmp(argv[arg], "-f") && arg + 1 < argc)
			fps = atoi(argv[++arg]);
	}
	if (fps <= 0)
		fps = 30;

	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		fprintf(stderr, "提示: 没有 fbdev 就用 DRM/KMS 或 weston + kmssrc\n");
		return 1;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		perror("FBIOGET_*SCREENINFO");
		close(fd);
		return 1;
	}

	printf("fb: %ux%u, %u bpp, line_length=%u, smem_len=%u, %s\n",
	       var.xres, var.yres, var.bits_per_pixel, fix.line_length,
	       fix.smem_len, fix.id);

	if (var.bits_per_pixel != 16 && var.bits_per_pixel != 32) {
		fprintf(stderr, "只支持 16/32 bpp，当前 %u bpp\n", var.bits_per_pixel);
		close(fd);
		return 1;
	}

	map_len = fix.smem_len ? fix.smem_len : (size_t)fix.line_length * var.yres;
	fb = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap fb");
		close(fd);
		return 1;
	}

	/* 把固定色板转换成本屏幕的像素格式 */
	{
		uint32_t native[8];

		for (i = 0; i < 8; i++)
			native[i] = pack_color(&var, (colors[i] >> 16) & 0xff,
					       (colors[i] >> 8) & 0xff, colors[i] & 0xff);

		for (i = 0; frames == 0 || i < frames; i++) {
			uint32_t bar_w = var.xres / 8;
			uint32_t box_x = (uint32_t)i * 8 % var.xres;
			uint32_t b;

			for (b = 0; b < 8; b++)
				fill_rect(fb, &var, &fix, b * bar_w, 0, bar_w,
					  var.yres, native[b]);

			/* 移动的白框：直观看到帧在刷新 */
			fill_rect(fb, &var, &fix, box_x, var.yres / 2 - 40,
				  80, 80, native[0]);

			usleep((useconds_t)(1000000 / fps));
		}
	}

	munmap(fb, map_len);
	close(fd);
	return 0;
}
