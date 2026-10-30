# study/ —— 从 Linux 基础到 RK3568 外设

这套目录是按你最终目标拆出来的学习路径，**不依赖 ROS2/YOLO**，先把地基打牢。

```
study/
├── linux-basics/            第 1 阶段：Linux 应用开发基础
│   ├── 01_read_sysfs.c      open/read/close、/sys、/proc
│   ├── 02_write_sysfs.c     写 sysfs、错误处理、权限
│   ├── 03_read_input.c      /dev/input + select + struct input_event
│   ├── 04_mmap_copy.c       mmap 文件拷贝
│   ├── 05_popen_meminfo.c   system/popen/sscanf
│   └── 06_ioctl_vendor.c    字符设备 + ioctl
│
└── embedded-peripherals/    第 2 阶段：控制线 + 视觉线外设
    ├── 01_uart_protocol.c   UART 帧协议 + CRC16 + 时延
    ├── 02_timing_jitter.c   周期抖动测量
    ├── 03_ringbuf_pipeline.c 采集/处理流水线骨架
    ├── 04_v4l2_capture.c    V4L2 采集（NV12/YUYV/MJPEG）
    ├── 05_fb_draw.c         /dev/fb0 屏幕输出
    ├── 06_rga_scale.c       RGA 硬件缩放
    ├── 07_mpp_encode.c      MPP H.264 硬编码
    └── 08_dmabuf_probe.c    DMA-BUF 能力探测
```

## 对应你的两个项目

| 项目 | 需要练的模块 | 对应例子 |
|---|---|---|
| RK3568 + STM32 灵巧手 | 串口协议、CRC、超时、时延/抖动 | `embedded-peripherals/01、02、03` |
| 边缘视觉节点 | V4L2、屏幕、RGA、MPP、DMA-BUF | `embedded-peripherals/04~08` |

## 建议顺序

1. `linux-basics`：01 -> 06，每课都上板跑一次。
2. `embedded-peripherals`：01 -> 02 -> 03（控制线）。
3. `embedded-peripherals`：04 -> 05 -> 06 -> 07 -> 08（视觉线）。
4. 把 04->06->07 串成脚本，测端到端延迟。
5. 再进入 RTSP、DMA-BUF 低拷贝、ROS2、YOLO。

## 编译

```bash
# 基础
make -C study/linux-basics

# 外设
make -C study/embedded-peripherals            # 基础例子
make -C study/embedded-peripherals check-media # RGA/MPP 语法检查
```

两套工程的 `README.md` 里有完整的上板命令、练习任务和常见坑。
