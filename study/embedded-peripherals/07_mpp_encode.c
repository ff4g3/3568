/*
 * 07_mpp_encode.c —— 用 MPP 把 NV12 帧硬编码成 H.264
 *
 * 对应你项目里的 "MPP 硬编码"。输入 NV12，输出 Annex-B 的 .h264。
 *
 * 知识点：
 *   1. MPP 编码五步：
 *        mpp_create -> mpp_init(ENC) -> MPP_ENC_GET_CFG/SET_CFG
 *        -> encode_put_frame -> encode_get_packet
 *   2. 输入帧放在 mpp_buffer（DRM 内存）里，MPP 直接读
 *   3. 输出 packet 的 pos/length 才是有效数据；每次 set_length(0) 复位
 *   4. SPS/PPS 用 MPP_ENC_GET_HDR_SYNC 提前取出，供 RTSP codec_data 使用
 *   5. stride 按 16 对齐；NV12 的 UV 偏移是 hor_stride * ver_stride
 *
 * 用法：
 *   ./07_mpp_encode -w 1280 -h 720 -n 60 -o out.h264
 *   ./07_mpp_encode -i in.nv12 -w 1280 -h 720 -n 60 -o out.h264
 *
 * 编译：需要镜像里的 librockchip_mpp（Buildroot 包 rockchip-mpp）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

#include "rk_mpi.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "mpp_buffer.h"
#include "mpp_meta.h"
#include "rk_venc_cfg.h"
#include "rk_venc_cmd.h"

#define ALIGN_16(x)	(((x) + 15) & ~15)

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static void fill_frame(uint8_t *buf, int w, int h, int hor, int ver, int idx)
{
	int x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			buf[y * hor + x] = (uint8_t)((x + y + idx * 4) & 0xff);
	memset(buf + (size_t)hor * ver, 128, (size_t)hor * ver / 2);
}

static int read_frame(FILE *fp, uint8_t *buf, int w, int h, int hor, int ver)
{
	int y;

	for (y = 0; y < h; y++) {
		if (fread(buf + (size_t)y * hor, 1, (size_t)w, fp) != (size_t)w)
			return -1;
	}
	if (fread(buf + (size_t)hor * ver, 1, (size_t)w * h / 2, fp) !=
	    (size_t)w * h / 2)
		return -1;
	return 0;
}

static int write_packet(FILE *fp, MppPacket packet, uint64_t *bytes)
{
	void *pos = mpp_packet_get_pos(packet);
	size_t len = mpp_packet_get_length(packet);

	if (pos && len) {
		if (fwrite(pos, 1, len, fp) != len)
			return -1;
		*bytes += len;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	const char *out_path = "out.h264";
	const char *in_path = NULL;
	int w = 1280, h = 720, frames = 60, fps = 30, bps = 4000000;
	int hor, ver, i;
	size_t frm_size;
	MppCtx ctx = NULL;
	MppApi *mpi = NULL;
	MppEncCfg cfg = NULL;
	MppBufferGroup grp = NULL;
	MppBuffer frm_buf = NULL, pkt_buf = NULL;
	MppFrame frame = NULL;
	MppPacket packet = NULL;
	MppPacket hdr = NULL;
	MppEncHeaderMode hdr_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
	RK_S32 timeout_ms = 2000;
	FILE *out = NULL, *in = NULL;
	uint64_t bytes = 0, t_start, t_end;
	MPP_RET ret = MPP_OK;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-i") && i + 1 < argc)
			in_path = argv[++i];
		else if (!strcmp(argv[i], "-o") && i + 1 < argc)
			out_path = argv[++i];
		else if (!strcmp(argv[i], "-w") && i + 1 < argc)
			w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-h") && i + 1 < argc)
			h = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-b") && i + 1 < argc)
			bps = atoi(argv[++i]);
		else {
			fprintf(stderr, "用法: %s [-i in.nv12] -w 1280 -h 720 "
				"-n 60 -b 4000000 -o out.h264\n", argv[0]);
			return 1;
		}
	}
	if (w <= 0 || h <= 0 || frames <= 0 || fps <= 0) {
		fprintf(stderr, "参数错误\n");
		return 1;
	}

	hor = ALIGN_16(w);
	ver = ALIGN_16(h);
	frm_size = (size_t)hor * ver * 3 / 2;

	out = fopen(out_path, "wb");
	if (!out) {
		fprintf(stderr, "fopen %s: %s\n", out_path, strerror(errno));
		return 1;
	}
	if (in_path) {
		in = fopen(in_path, "rb");
		if (!in) {
			fprintf(stderr, "fopen %s: %s\n", in_path, strerror(errno));
			fclose(out);
			return 1;
		}
	}

	ret = mpp_create(&ctx, &mpi);
	if (ret != MPP_OK) {
		fprintf(stderr, "mpp_create: %d\n", ret);
		goto out_files;
	}

	mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout_ms);
	ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
	if (ret != MPP_OK) {
		fprintf(stderr, "mpp_init: %d\n", ret);
		goto destroy;
	}

	ret = mpp_enc_cfg_init(&cfg);
	if (ret != MPP_OK) {
		fprintf(stderr, "mpp_enc_cfg_init: %d\n", ret);
		goto destroy;
	}
	ret = mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
	if (ret != MPP_OK) {
		fprintf(stderr, "MPP_ENC_GET_CFG: %d\n", ret);
		goto deinit_cfg;
	}

	mpp_enc_cfg_set_s32(cfg, "prep:width", w);
	mpp_enc_cfg_set_s32(cfg, "prep:height", h);
	mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", hor);
	mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", ver);
	mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);

	mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bps * 17 / 16);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bps * 15 / 16);
	mpp_enc_cfg_set_s32(cfg, "rc:gop", fps * 2);

	mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
	mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
	mpp_enc_cfg_set_s32(cfg, "h264:level", 40);

	ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
	if (ret != MPP_OK) {
		fprintf(stderr, "MPP_ENC_SET_CFG: %d\n", ret);
		goto deinit_cfg;
	}
	ret = mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &hdr_mode);
	if (ret != MPP_OK) {
		fprintf(stderr, "MPP_ENC_SET_HEADER_MODE: %d\n", ret);
		goto deinit_cfg;
	}

	ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM);
	if (ret != MPP_OK) {
		fprintf(stderr, "mpp_buffer_group_get_internal: %d\n", ret);
		goto deinit_cfg;
	}
	ret = mpp_buffer_get(grp, &frm_buf, frm_size);
	if (ret != MPP_OK) {
		fprintf(stderr, "mpp_buffer_get(frame): %d\n", ret);
		goto put_group;
	}
	ret = mpp_buffer_get(grp, &pkt_buf, frm_size + 4096);
	if (ret != MPP_OK) {
		fprintf(stderr, "mpp_buffer_get(packet): %d\n", ret);
		goto put_frm;
	}

	/* 先取 SPS/PPS（Annex-B），RTSP 的 codec_data 就需要它 */
	mpp_packet_init_with_buffer(&hdr, pkt_buf);
	mpp_packet_set_length(hdr, 0);
	ret = mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, hdr);
	if (ret == MPP_OK) {
		write_packet(out, hdr, &bytes);
	} else {
		fprintf(stderr, "MPP_ENC_GET_HDR_SYNC: %d（继续）\n", ret);
		ret = MPP_OK;
	}
	mpp_packet_deinit(&hdr);

	printf("开始编码 %dx%d (stride %dx%d), %d 帧, %d bps\n",
	       w, h, hor, ver, frames, bps);

	t_start = now_us();
	for (i = 0; i < frames; i++) {
		uint8_t *buf = mpp_buffer_get_ptr(frm_buf);
		uint64_t frame_start = now_us();
		MppMeta meta;

		if (in) {
			if (read_frame(in, buf, w, h, hor, ver) < 0) {
				fprintf(stderr, "第 %d 帧读取失败，提前结束\n", i);
				break;
			}
		} else {
			fill_frame(buf, w, h, hor, ver, i);
		}

		ret = mpp_frame_init(&frame);
		if (ret != MPP_OK) {
			fprintf(stderr, "mpp_frame_init: %d\n", ret);
			break;
		}
		mpp_frame_set_width(frame, w);
		mpp_frame_set_height(frame, h);
		mpp_frame_set_hor_stride(frame, hor);
		mpp_frame_set_ver_stride(frame, ver);
		mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
		mpp_frame_set_eos(frame, 0);
		mpp_frame_set_buffer(frame, frm_buf);

		mpp_packet_init_with_buffer(&packet, pkt_buf);
		mpp_packet_set_length(packet, 0);
		meta = mpp_frame_get_meta(frame);
		if (meta)
			mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet);

		ret = mpi->encode_put_frame(ctx, frame);
		mpp_frame_deinit(&frame);
		frame = NULL;
		if (ret != MPP_OK) {
			fprintf(stderr, "encode_put_frame: %d\n", ret);
			mpp_packet_deinit(&packet);
			break;
		}

		for (;;) {
			MppPacket out_pkt = NULL;
			int eos;

			ret = mpi->encode_get_packet(ctx, &out_pkt);
			if (ret != MPP_OK || !out_pkt) {
				if (ret != MPP_OK)
					fprintf(stderr, "encode_get_packet: %d\n", ret);
				break;
			}
			if (write_packet(out, out_pkt, &bytes) < 0) {
				fprintf(stderr, "写输出文件失败\n");
				mpp_packet_deinit(&out_pkt);
				ret = MPP_NOK;
				break;
			}
			eos = mpp_packet_get_eos(out_pkt);
			mpp_packet_deinit(&out_pkt);
			packet = NULL;
			if (eos)
				break;
		}
		if (ret != MPP_OK)
			break;

		/* 每帧打印一次耗时，便于发现抖动 */
		if ((i % 10) == 0 || i == frames - 1)
			printf("帧 %d: %.2f ms\n", i, (now_us() - frame_start) / 1000.0);
	}

	t_end = now_us();
	printf("\n编码结束: %d 帧, 输出 %llu 字节, 总耗时 %.1f ms, "
	       "平均 %.1f fps, %.1f kbps\n",
	       i, (unsigned long long)bytes, (t_end - t_start) / 1000.0,
	       i > 0 ? i * 1000000.0 / (t_end - t_start) : 0.0,
	       (t_end > t_start && i > 0) ?
	       bytes * 8.0 * 1000000.0 / (t_end - t_start) / 1000.0 : 0.0);
	/* 发送 EOS，flush 最后一个 GOP */
	if (ret == MPP_OK) {
		ret = mpp_frame_init(&frame);
		if (ret == MPP_OK) {
			MppMeta meta;

			mpp_frame_set_width(frame, w);
			mpp_frame_set_height(frame, h);
			mpp_frame_set_hor_stride(frame, hor);
			mpp_frame_set_ver_stride(frame, ver);
			mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
			mpp_frame_set_eos(frame, 1);
			mpp_frame_set_buffer(frame, NULL);

			mpp_packet_init_with_buffer(&packet, pkt_buf);
			mpp_packet_set_length(packet, 0);
			meta = mpp_frame_get_meta(frame);
			if (meta)
				mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet);

			ret = mpi->encode_put_frame(ctx, frame);
			mpp_frame_deinit(&frame);
			frame = NULL;
			if (ret == MPP_OK) {
				for (;;) {
					MppPacket out_pkt = NULL;
					int eos;

					ret = mpi->encode_get_packet(ctx, &out_pkt);
					if (ret != MPP_OK || !out_pkt) {
						ret = MPP_OK;
						break;
					}
					write_packet(out, out_pkt, &bytes);
					eos = mpp_packet_get_eos(out_pkt);
					mpp_packet_deinit(&out_pkt);
					packet = NULL;
					if (eos)
						break;
				}
			}
		}
	}

	if (packet) {
		mpp_packet_deinit(&packet);
		packet = NULL;
	}
	if (frame) {
		mpp_frame_deinit(&frame);
		frame = NULL;
	}
	if (pkt_buf) {
		mpp_buffer_put(pkt_buf);
		pkt_buf = NULL;
	}
	if (frm_buf) {
		mpp_buffer_put(frm_buf);
		frm_buf = NULL;
	}
	if (grp) {
		mpp_buffer_group_put(grp);
		grp = NULL;
	}
	goto deinit_cfg;

put_frm:
	if (frm_buf)
		mpp_buffer_put(frm_buf);
put_group:
	if (grp)
		mpp_buffer_group_put(grp);
deinit_cfg:
	if (cfg)
		mpp_enc_cfg_deinit(cfg);
destroy:
	if (ctx)
		mpp_destroy(ctx);
out_files:
	if (in)
		fclose(in);
	if (out)
		fclose(out);
	return ret == MPP_OK ? 0 : 1;
}
