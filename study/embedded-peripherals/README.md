# 嵌入式 Linux 外设进阶练习（RK3568）

这套例子承接 `study/linux-basics/`，目标是把你的两个项目拆成**可以单独跑通的小实验**：

- **控制线**：UART 帧协议 -> 周期抖动 -> 采集/处理流水线（对应 RK3568 + STM32 灵巧手）
- **视觉线**：V4L2 采集 -> 屏幕输出 -> RGA 缩放 -> MPP 编码 -> DMA-BUF 探测（对应边缘视觉节点）

> 本阶段**不涉及 ROS2 / YOLO / RTSP 业务**，先把 Linux 应用开发和摄像头/屏幕外设打牢。
> RTSP 和完整低拷贝流水线放在文末"下一步"。

---

## 1. 文件与依赖

| 文件 | 作用 | 依赖 |
|---|---|---|
| `01_uart_protocol.c` | termios + 帧协议 + CRC16 + 时延统计，带 pty 自测 | libc |
| `02_timing_jitter.c` | 周期任务抖动测量（p50/p99/max、SCHED_FIFO） | libc, libm |
| `03_ringbuf_pipeline.c` | 缓冲池 + 生产者/消费者线程 + 端到端延迟 | libc, pthread |
| `04_v4l2_capture.c` | V4L2 mmap 采集，保存 NV12/YUYV/MJPEG 并测 fps | libc, 内核头 |
| `05_fb_draw.c` | `/dev/fb0` mmap 画彩条 + 移动方块 | libc, 内核头 |
| `06_rga_scale.c` | RGA 硬件缩放 NV12，统计单次耗时 | `librga` |
| `07_mpp_encode.c` | MPP 硬编码 H.264（NV12 -> Annex-B） | `librockchip_mpp` |
| `08_dmabuf_probe.c` | V4L2 EXPBUF / dma-heap，验证 fd 可 mmap | libc, 内核头 |

`01/02/03` 在 PC 上就能跑；`04/05/08` 能编译，但要有摄像头/屏幕才能看到效果；
`06/07` 需要板子镜像里的 `librga`、`librockchip_mpp`。

---

## 2. 编译与上板

```bash
cd /home/lhy/sdk/linux/rk356x-linux-2026092909/study/embedded-peripherals

# 基础例子（aarch64 静态链接，便于直接丢到板子）
make

# 只做 RGA/MPP 代码的语法检查（不需要库）
make check-media

# 板端库不在默认路径时
make media MEDIA_LDFLAGS=-L/your/lib/dir

# 本机验证逻辑（会覆盖 aarch64 产物，之后记得重新 make）
make host
```

上板（三选一）：

```bash
adb push 01_uart_protocol /userdata/
adb shell "chmod +x /userdata/01_uart_protocol && /userdata/01_uart_protocol --selftest 500"

scp 0* root@<板子IP>:/userdata/
```

---

## 3. 控制线怎么练（对应项目 1）

### 3.1 UART 协议

```bash
# PC 上先跑通协议和时延（不需要板子）
./01_uart_protocol --selftest 1000

# 板子上发一帧到 STM32（假设接 /dev/ttyS3）
./01_uart_protocol -d /dev/ttyS3 -b 115200 send '{"j1":30,"j2":-10}'

# 接收 STM32 上报
./01_uart_protocol -d /dev/ttyS3 -b 115200 recv 30
```

接线/配置要点：

1. **TX/RX 交叉**，共地；3.3V TTL，不要直接接 5V。
2. 先确认串口没被控制台占用：`cat /proc/cmdline`、`ls -l /dev/ttyS*`。
3. 权限：`ls -l /dev/ttyS3`，普通用户可加入 `dialout` 组或直接用 root。
4. 流控：默认无硬件流控；长线/高波特率再考虑 `CRTSCTS`。
5. 帧格式、CRC 多项式、字节序必须和 STM32 端完全一致，最好把协议写成一份文档。

**练习**：把 payload 换成固定长度二进制结构体（`#pragma pack` 或手动序列化），
再加一个 `seq` 重发/超时机制，这就是一个可用的关节控制协议。

### 3.2 周期抖动

```bash
./02_timing_jitter 5000 2000       # 5ms 控制周期，2000 次
./02_timing_jitter 5000 2000 300   # 最后 300us 忙等，对比 p99
```

关注 **p99 和 max**，不要只看 avg。控制环里 1 次 20ms 的抖动就可能让机械手抖一下。

**练习**：把 `period_us` 改成 1000/2000/5000，对比有/无 `--spin`、有/无 `SCHED_FIFO`
的 p99；再到板子上跑一遍（需 root 才能设实时优先级）。

### 3.3 流水线骨架

```bash
./03_ringbuf_pipeline 5 30 5    # 5 秒，30fps，处理 5ms
```

它演示了"固定缓冲池 + 就绪队列 + 时间戳"，这是后面 V4L2->RGA->MPP 流水线的骨架。
**练习**：把处理耗时改成 50ms，观察 `丢弃` 计数和延迟分布——这就是背压。

---

## 4. 视觉线怎么练（对应项目 2）

### 4.1 先确认摄像头

```bash
ls -l /dev/video*
v4l2-ctl --list-devices
./04_v4l2_capture --list /dev/video0
```

- **USB 摄像头**：一般直接出 YUYV/MJPEG，最适合入门。
- **MIPI 摄像头**：RK3568 上通常要先配置 media pipeline / rkaiq，
  直接打开 `/dev/videoX` 可能一直超时；先用 USB 摄像头把流程跑通。
- 确认驱动/设备：`dmesg | grep -i -E 'camera|v4l2|isp'`。

### 4.2 采集

```bash
./04_v4l2_capture -d /dev/video0 -W 1280 -H 720 -f NV12 -n 30 -o /userdata/cap
```

产物：

- `frame_0000.nv12`：原始 NV12，喂给 RGA/MPP
- `frame_0000.pgm`：Y 平面预览，PC 上直接能看
- YUYV 会额外生成 `.ppm`；MJPEG 直接存 `.jpg`

**练习**：把 `-n` 改大，观察 fps；把 `-f` 换成 MJPEG，比较 `bytesused` 和 `sizeimage`。

### 4.3 屏幕

```bash
./05_fb_draw -d /dev/fb0 -n 60
```

没有 `/dev/fb0` 说明系统只走 DRM/KMS，可以：

- 用 weston（Wayland）+ `kmssrc`（`external/gstreamer-rockchip/gst/kmssrc`）
- 或者后续直接学 DRM dumb buffer / atomic modesetting

**练习**：把彩条换成"左右移动的方块"，再叠加一行文字（先用点阵字模即可）。

### 4.4 RGA 缩放

```bash
./06_rga_scale -i /userdata/cap/frame_0000.nv12 -w 1280 -h 720 -W 640 -H 360 \
               -o /userdata/out.nv12 -n 100
```

关注输出的 `min/avg/p50/p99/max`。RGA 正常应该在**几百微秒到 1ms**量级（取决于分辨率）。

**练习**：对比 1080p->720p 和 720p->1080p；把 `imresize` 换成 `imcrop`/`imrotate`。

### 4.5 MPP 编码

```bash
./07_mpp_encode -i /userdata/cap/frame_0000.nv12 -w 1280 -h 720 -n 60 \
                -b 4000000 -o /userdata/out.h264
```

把 `out.h264` 拷回 PC：

```bash
ffplay out.h264
# 或者
ffmpeg -i out.h264 -f null -
```

**练习**：比较 1Mbps 和 8Mbps 的输出大小；把 `rc:mode` 改成 VBR 再对比。

### 4.6 DMA-BUF 探测（低拷贝的第一步）

```bash
./08_dmabuf_probe -d /dev/video0 -W 1280 -H 720
```

它做两件事：

1. `VIDIOC_EXPBUF` 把 V4L2 缓冲区导出成 dma-buf fd，并 mmap 验证；
2. 尝试从 `/dev/dma_heap/system` 申请 dma-buf。

如果 `VIDIOC_EXPBUF` 失败，说明驱动/内核没开 dma-buf 导出，低拷贝流水线要先解决这个。

**真正的零拷贝链路**（下一步再做）：

```
V4L2 buffer --EXPBUF--> dmabuf fd
                         |-- importbuffer_fd() --> RGA 缩放（fd 进 fd 出）
                         |-- MPP 外部 buffer    --> H.264
                         |-- DRM plane fd       --> 屏幕显示
```

全链路只传 fd 和元数据，不 `memcpy` 像素。

---

## 5. 建议的练习顺序（约 2~3 周）

| 顺序 | 内容 | 验收标准 |
|---|---|---|
| 1 | 01 pty 自测 | 1000 帧 0 丢失，知道 RTT 的 p99 |
| 2 | 01 接 STM32 | 能发关节目标、收到 ACK，CRC 错误能丢弃 |
| 3 | 02 抖动 | 能解释 avg/p99/max 差异，会对比 --spin |
| 4 | 03 流水线 | 能通过改处理耗时制造丢弃并解释 |
| 5 | 04 采集 | 保存 NV12 + PGM，PC 能看画面，fps 正确 |
| 6 | 05 屏幕 | fb0 上看到彩条和移动方块 |
| 7 | 06 RGA | 输出尺寸正确，p99 有数据 |
| 8 | 07 MPP | out.h264 能被 ffplay 播放 |
| 9 | 08 DMA-BUF | EXPBUF 成功，能说出 fd 如何传给 RGA/MPP |
| 10 | 组合 | 04->06->07 串成一条命令/小脚本，测端到端延迟 |

---

## 6. 测量方法（项目 1 的"端到端延迟/抖动"）

1. **时钟源**：统一用 `CLOCK_MONOTONIC`；跨设备对时可以看 PTP/NTP，但不要用 `gettimeofday` 做差值。
2. **打点位置**：
   - T0：视觉/上位机产生目标
   - T1：UART 发出第一字节
   - T2：STM32 收到并回 ACK（STM32 自己打点，或回环测量）
   - T3：舵机开始动作（可用电流/编码器信号）
3. **统计**：至少 1000 次，报告 min/avg/p50/p95/p99/max，不要只报平均。
4. **隔离干扰**：`taskset` 绑核、关掉调试打印、避免在控制线程里 malloc。
5. **实时性**：控制线程可尝试 `SCHED_FIFO`（需 root），并把忙等放在周期末尾。

---

## 7. 常见坑

1. `make media` 链接失败：镜像里没有 `librga` / `librockchip_mpp`，或路径不对。
2. V4L2 一直超时：MIPI 摄像头没配 pipeline；换 USB 摄像头验证。
3. NV12 花屏：`stride` 不是 width，保存/缩放时要按 `bytesperline` 处理。
4. RGA 报参数错：宽高/stride 没按 16 对齐。
5. MPP 输出无法播放：没写 SPS/PPS，或漏了 EOS flush。
6. fb0 不存在：系统只走 DRM/KMS。
7. 串口收到乱码：波特率/校验/地线/电平不对，或调试串口被占用。
8. `SCHED_FIFO` 失败：普通用户没权限，用 root 或 `setcap`。
9. dma-buf 导出失败：内核/驱动没开 `VIDIOC_EXPBUF`。
10. 不要用 `printf` 打高频日志，`printf` 本身就会引入毫秒级抖动。

---

## 8. 下一步（等基础打牢再做）

- **RTSP 推流**：用 `external/common_algorithm/misc/include/rtsp_demo.h`（已带
  `aarch64` 的 `librtsp.a`）：
  `rtsp_new_demo(8554)` -> `rtsp_new_session("/live")` ->
  `rtsp_set_video(H264, sps_pps, len)` -> 每帧 `rtsp_tx_video(...)`。
  07 的输出天然就是 Annex-B，接上去即可。
- **完整低拷贝流水线**：V4L2 EXPBUF -> RGA `importbuffer_fd` -> MPP 外部 buffer ->
  RTSP/DRM，全程传 fd；用 02/03 的方法测每一级延迟。
- **ROS2**：等 UART 协议和视觉流水线都稳定后，再把节点包一层 ROS2 topic，
  否则调试会被框架噪音淹没。
- **YOLO**：先保证 NV12 帧能稳定拿到、能缩放、能编码，再上 `rknpu2` + INT8 模型。
