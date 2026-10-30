/*
 * 04_mmap_copy.c —— 用 mmap 做文件拷贝
 *
 * 知识点：
 *   1. mmap() 把文件映射进进程地址空间，之后像访问数组一样访问文件
 *   2. mmap 的 offset 必须是页大小(通常 4096)的整数倍
 *   3. MAP_PRIVATE / MAP_SHARED 的区别：
 *        MAP_PRIVATE 改动不写回文件（写时复制）
 *        MAP_SHARED  改动会写回文件
 *   4. munmap() 释放映射，close() 之后映射仍然有效（映射有自己的引用）
 *
 * 用法：
 *   ./04_mmap_copy 源文件 目标文件
 *
 * 进阶：把源文件换成 /dev/fb0、/dev/mem 或 DRM/DMA 的 fd，
 *       就是显示、寄存器操作、零拷贝视频的雏形（参考 external/uvc_app/main.c）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(int argc, char *argv[])
{
	const char *src, *dst;
	struct stat st;
	void *addr;
	off_t off;
	int in_fd, out_fd, ret = 1;

	if (argc != 3) {
		fprintf(stderr, "用法: %s <源文件> <目标文件>\n", argv[0]);
		return 1;
	}
	src = argv[1];
	dst = argv[2];

	in_fd = open(src, O_RDONLY | O_CLOEXEC);
	if (in_fd < 0) {
		fprintf(stderr, "open %s: %s\n", src, strerror(errno));
		return 1;
	}
	if (fstat(in_fd, &st) < 0) {
		perror("fstat");
		close(in_fd);
		return 1;
	}
	if (st.st_size == 0) {
		fprintf(stderr, "%s 大小为 0。\n", src);
		fprintf(stderr,
			"提示: /proc、/sys 下很多文件 stat 大小为 0，不能用 mmap，\n"
			"      要用 read()（见 01_read_sysfs.c）。\n");
		close(in_fd);
		return 1;
	}

	/*
	 * 映射长度按页向上取整；
	 * 映射偏移 0 是最简单的，任意偏移时要做页对齐处理。
	 */
	addr = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, in_fd, 0);
	if (addr == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		close(in_fd);
		return 1;
	}
	/* 映射建立后，源 fd 就可以关了 */
	close(in_fd);

	out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out_fd < 0) {
		fprintf(stderr, "open %s: %s\n", dst, strerror(errno));
		goto out_unmap;
	}

	/* 注意：文件可能很大，write 不保证一次写完，要循环 */
	for (off = 0; off < st.st_size; ) {
		ssize_t n = write(out_fd, (char *)addr + off, (size_t)(st.st_size - off));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "write: %s\n", strerror(errno));
			close(out_fd);
			goto out_unmap;
		}
		off += n;
	}

	close(out_fd);
	printf("拷贝完成: %s (%lld 字节) -> %s\n",
	       src, (long long)st.st_size, dst);
	ret = 0;

out_unmap:
	if (munmap(addr, (size_t)st.st_size) < 0)
		perror("munmap");
	return ret;
}
