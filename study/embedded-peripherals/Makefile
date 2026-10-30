# 嵌入式外设练习工程 —— 交叉编译 Makefile
#
#   make            编译基础例子（UART/时延/流水线/V4L2/fb/dma-buf，静态链接）
#   make media      编译 RGA/MPP 例子（需要板端库，见 README）
#   make check-media 只做语法检查（不需要库）
#   make host       用本机 gcc 编译基础例子（会覆盖 aarch64 产物）
#   make clean

SDK_DIR      ?= $(abspath $(CURDIR)/../..)
TOOLCHAIN    ?= $(SDK_DIR)/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin
CROSS_COMPILE ?= $(TOOLCHAIN)/aarch64-none-linux-gnu-
# CC 是 make 内置变量，必须用 := 才能覆盖；命令行 CC=gcc 仍然优先
CC := $(CROSS_COMPILE)gcc

CFLAGS   ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS  ?= -s
BASE_STATIC ?= -static

RGA_DIR  ?= $(SDK_DIR)/external/linux-rga
RGA_CFLAGS := -I$(RGA_DIR)/im2d_api -I$(RGA_DIR)/include
MPP_DIR  ?= $(SDK_DIR)/external/mpp
MPP_CFLAGS := -I$(MPP_DIR)/inc

# 板端库如果不在默认搜索路径，可加：make media MEDIA_LDFLAGS="-L/path/to/lib"
MEDIA_LDFLAGS ?=

BASE  := 01_uart_protocol 02_timing_jitter 03_ringbuf_pipeline \
         04_v4l2_capture 05_fb_draw 08_dmabuf_probe
MEDIA := 06_rga_scale 07_mpp_encode

.PHONY: all base media check-media host clean list help

all: base

base: $(BASE)

media: $(MEDIA)

$(BASE): %: %.c
	$(CC) $(CFLAGS) $(BASE_STATIC) -o $@ $< $(LDFLAGS) -lm -pthread

06_rga_scale: 06_rga_scale.c
	$(CC) $(CFLAGS) $(RGA_CFLAGS) -o $@ $< $(LDFLAGS) $(MEDIA_LDFLAGS) -lrga -pthread

07_mpp_encode: 07_mpp_encode.c
	$(CC) $(CFLAGS) $(MPP_CFLAGS) -o $@ $< $(LDFLAGS) $(MEDIA_LDFLAGS) -lrockchip_mpp -pthread

# 没有板端库时，至少验证代码能通过编译
check-media:
	$(CC) $(CFLAGS) $(RGA_CFLAGS) -fsyntax-only 06_rga_scale.c
	$(CC) $(CFLAGS) $(MPP_CFLAGS) -fsyntax-only 07_mpp_encode.c

host:
	$(MAKE) CC=gcc BASE_STATIC= all

clean:
	rm -f $(BASE) $(MEDIA)

list:
	@echo "base : $(BASE)"
	@echo "media: $(MEDIA)"

help:
	@echo "make              基础例子（aarch64 静态）"
	@echo "make media        RGA/MPP 例子（需要 librga/librockchip_mpp）"
	@echo "make check-media  只做语法检查"
	@echo "make host         本机 gcc 编译基础例子"
	@echo "make clean        清理"
