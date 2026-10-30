# Linux 基础练习工程 —— 交叉编译 Makefile
#
# 用法：
#   make                # 用 SDK 自带 aarch64 工具链静态编译全部例子
#   make host           # 用本机 gcc 编译（只能验证语法/在 PC 上跑部分例子）
#   make clean
#
# 默认工具链来自本 SDK：
#   prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu
# 如果你的 SDK 路径不同，可以覆盖 SDK_DIR / CROSS_COMPILE：
#   make SDK_DIR=/path/to/sdk
#   make CROSS_COMPILE=aarch64-linux-gnu-

SDK_DIR      ?= $(abspath $(CURDIR)/../..)
TOOLCHAIN    ?= $(SDK_DIR)/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin
CROSS_COMPILE ?= $(TOOLCHAIN)/aarch64-none-linux-gnu-
# 注意：CC 是 make 的内置变量（默认 cc），用 ?= 无法覆盖，必须用 :=。
# 命令行传入的变量优先级最高，所以 "make CC=gcc" / "make host" 仍然有效。
CC := $(CROSS_COMPILE)gcc

CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
# 静态链接：板子上不需要装对应版本的 libc，也不用手动拷 .so
STATIC  ?= -static
# -s：去掉符号/调试段，减小体积（想用 gdb 调试时把它去掉）
LDFLAGS ?= -s

TARGETS := 01_read_sysfs 02_write_sysfs 03_read_input 04_mmap_copy 05_popen_meminfo 06_ioctl_vendor

.PHONY: all host clean list help

all: $(TARGETS)

# 每个 .c 直接生成同名可执行文件
$(TARGETS): %: %.c
	$(CC) $(CFLAGS) $(STATIC) -o $@ $< $(LDFLAGS)

# 用本机编译器验证语法；sysfs/input 等例子在 x86 上也能跑一部分
host:
	$(MAKE) CC=gcc STATIC= all

clean:
	rm -f $(TARGETS)

list:
	@echo "目标：$(TARGETS)"

help:
	@echo "make        交叉编译（aarch64，静态链接）"
	@echo "make host   本机编译"
	@echo "make clean  清理"
