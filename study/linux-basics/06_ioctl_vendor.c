/*
 * 06_ioctl_vendor.c —— 字符设备 + ioctl 只读示例
 *
 * 知识点：
 *   1. 设备文件（/dev/xxx）用 open 打开，用 ioctl 发"命令"
 *   2. ioctl(fd, 命令号, 参数指针)，命令号由 _IO/_IOR/_IOW/_IOWR 宏生成
 *   3. 用户态和内核态必须对同一个 struct 布局达成一致，否则数据错乱
 *   4. 这里只做"读"，不会修改板子上的 SN/MAC 等关键信息
 *
 * 参考代码：external/rktoolkit/vendor_storage.c:121-196（含读写）
 *   内核驱动：kernel/drivers/soc/rockchip/rk_vendor_storage.c
 *
 * 用法：
 *   ./06_ioctl_vendor        # 读 ID=1（VENDOR_SN_ID）
 *   ./06_ioctl_vendor 2      # 读 ID=2（VENDOR_WIFI_MAC_ID）
 *
 * 需要：root 权限 + 内核已启用 CONFIG_ROCKCHIP_VENDOR_STORAGE
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define VENDOR_MAX_SIZE	1024
#define VENDOR_REQ_TAG	0x56524551	/* 必须是这个魔数，否则内核拒绝 */

#define VENDOR_READ_IO	_IOW('v', 0x01, unsigned int)
#define VENDOR_WRITE_IO	_IOW('v', 0x02, unsigned int)

/*
 * 这个结构体必须和内核里的 struct rk_vendor_req 完全一致：
 *   u32 tag; u16 id; u16 len; u8 data[1024];
 */
struct rk_vendor_req {
	unsigned int	tag;
	unsigned short	id;
	unsigned short	len;
	unsigned char	data[VENDOR_MAX_SIZE];
};

static const char *vendor_id_name(unsigned int id)
{
	static const char *names[] = {
		"VENDOR_SN_ID", "VENDOR_WIFI_MAC_ID", "VENDOR_LAN_MAC_ID",
		"VENDOR_BT_MAC_ID", "VENDOR_HDCP_14_HDMI_ID", "VENDOR_HDCP_14_DP_ID",
		"VENDOR_HDCP_2x_ID", "VENDOR_DRM_KEY_ID", "VENDOR_PLAYREADY_Cert_ID",
		"VENDOR_ATTENTION_KEY_ID", "VENDOR_PLAYREADY_ROOT_KEY_0_ID",
		"VENDOR_PLAYREADY_ROOT_KEY_1_ID", "VENDOR_SENSOR_CALIBRATION_ID",
		"VENODR_RESERVE_ID_14", "VENDOR_IMEI_ID", "VENDOR_CUSTOM_ID",
	};

	if (id >= 1 && id <= sizeof(names) / sizeof(names[0]))
		return names[id - 1];
	return "CUSTOM";
}

int main(int argc, char *argv[])
{
	struct rk_vendor_req req;
	unsigned int id = (argc > 1) ? (unsigned int)strtoul(argv[1], NULL, 0) : 1;
	int fd, ret, i;

	fd = open("/dev/vendor_storage", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open /dev/vendor_storage: %s\n", strerror(errno));
		fprintf(stderr, "提示: 确认内核驱动已加载 (lsmod | grep vendor / ls /dev/vendor_storage)\n");
		return 1;
	}

	memset(&req, 0, sizeof(req));
	req.tag = VENDOR_REQ_TAG;
	req.id = (unsigned short)id;
	req.len = VENDOR_MAX_SIZE;

	ret = ioctl(fd, VENDOR_READ_IO, &req);
	if (ret) {
		fprintf(stderr, "ioctl(VENDOR_READ_IO) 失败: ret=%d errno=%s\n",
			ret, strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	printf("id=%u (%s) len=%u\n", id, vendor_id_name(id), req.len);

	if (req.len > VENDOR_MAX_SIZE)
		req.len = VENDOR_MAX_SIZE;

	/* 十六进制 + ASCII 两种方式打印，便于观察二进制数据 */
	for (i = 0; i < req.len; i++) {
		if (i % 16 == 0)
			printf("\n%04x: ", i);
		printf("%02x ", req.data[i]);
	}
	printf("\n\nASCII: ");
	for (i = 0; i < req.len; i++) {
		unsigned char c = req.data[i];

		putchar((c >= 0x20 && c < 0x7f) ? c : '.');
	}
	putchar('\n');

	return 0;
}
