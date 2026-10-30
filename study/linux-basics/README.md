# Linux 基础练习工程（RK356x SDK 配套）

这是给"嵌入式 Linux 初学者"准备的最小练习集：**不依赖 SDK 的 MPP/RGA/AIQ 等大库**，
只用 glibc，专注 `文件操作 -> 设备节点 -> ioctl -> mmap -> 进程/管道` 这一条主线。
每个例子都能单独看懂、单独运行，适合配合 `external/` 里的产品代码对照学习。

```
study/linux-basics/
├── Makefile
├── 01_read_sysfs.c      open/read/close 读 /sys、/proc（文本文件）
├── 02_write_sysfs.c     向 /sys 写字符串（控制 LED）
├── 03_read_input.c      /dev/input/event* + select + struct input_event
├── 04_mmap_copy.c       mmap 文件拷贝（mmap 入门）
├── 05_popen_meminfo.c   system()/popen()/sscanf 解析命令输出
└── 06_ioctl_vendor.c    字符设备 + ioctl 只读访问 vendor_storage
```

---

## 1. 编译

```bash
cd /home/lhy/sdk/linux/rk356x-linux-2026092909/study/linux-basics

# 用 SDK 自带工具链，静态编译（推荐）
make

# 检查产物架构，应该看到 ARM aarch64
file 01_read_sysfs

# 也可用本机 gcc 先验证语法（注意：会覆盖上面编出的 aarch64 程序，
# 验证完记得再执行一次 make 重新交叉编译）
make host
```

> 小技巧：`make host` 和 `make` 产物同名，二者会互相覆盖。
> 建议顺序：`make host` 验证逻辑 → `make clean all` 生成上板程序。

工具链位置（本 SDK 实测存在）：

```
prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc
```

想手动编译单个文件：

```bash
export PATH=$PWD/../../prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin:$PATH
aarch64-none-linux-gnu-gcc -static -O2 -Wall -o 01_read_sysfs 01_read_sysfs.c
```

## 2. 传到板子上运行

三种常见方式，任选一种：

```bash
# 方式 A：adb（Android/Debian 镜像打开 adb 时）
adb push 01_read_sysfs /userdata/
adb shell "chmod +x /userdata/01_read_sysfs && /userdata/01_read_sysfs"

# 方式 B：scp（板子和 PC 在同一网络，sshd 已开）
scp 01_read_sysfs root@<板子IP>:/userdata/

# 方式 C：U 盘 / TF 卡，直接 cp
```

> 板子根文件系统通常只读，可写目录用 `/userdata`、`/data`、`/tmp`。
> 需要 root 的例子（写 LED、ioctl）请用 root 登录。

---

## 3. 每个例子学什么 + 改造任务

| 例子 | 核心知识点 | 建议改造 |
|---|---|---|
| 01_read_sysfs | `open/read/close`、EINTR、字符串补 `\0`、sysfs 是文本 | 增加读 `/sys/class/leds/*/brightness`、格式化输出 JSON |
| 02_write_sysfs | 写 sysfs 必须"一次 write"、权限/只读文件系统错误处理 | 加 `--blink N`，循环亮灭，练习 `usleep()` 和信号退出 |
| 03_read_input | 字符设备二进制读、`select` 超时、`struct input_event` | 统计按键次数；用 `poll()` 重写一遍；支持多个 event |
| 04_mmap_copy | `mmap/munmap`、页对齐、`MAP_PRIVATE` vs `MAP_SHARED` | 改成 `MAP_SHARED` 后在映射里改一个字节，观察源文件变化 |
| 05_popen_meminfo | `system`/`popen`/`pclose`、`WEXITSTATUS`、`sscanf` | 读取 `/proc/stat` 计算 CPU 使用率 |
| 06_ioctl_vendor | `ioctl` 命令号、内核/用户态结构体一致、设备节点 | 加 `-w` 写自定义 ID（**先备份，别覆盖 SN/MAC**） |

对应 `external/` 里的"原版"代码：

- `external/rk_pcba_test/echo_emmc_test.c:51` —— `open/read/close` 读 sysfs
- `external/rk_pcba_test/echo_cpu_test.c:82` —— `fopen/fgets` + `system("echo ... > /sys/...")`
- `external/rk_pcba_test/echo_ddr_test.c:38` —— `popen` 读 `/proc/zoneinfo`
- `external/rk_pcba_test/echo_key_test.c:267` —— `select + read(struct input_event)`
- `external/rktoolkit/vendor_storage.c:121` —— `open(/dev/vendor_storage) + ioctl`
- `external/uvc_app/main.c:35` —— `drm_alloc/drm_map_buffer`（DMA buffer + mmap 的雏形）

> 注意：`rk_pcba_test` 里的 `echo_*` 是产线测试程序，运行结果会通过 TCP
> 上报给 PCBA 测试服务器，**不要直接拿来当学习程序跑**，摘抄其中文件操作片段即可。
> 另外这些源文件注释是 GBK 编码，直接看会乱码，可以：
> `iconv -f GB18030 -t UTF-8 echo_cpu_test.c | less`

---

## 4. 建议的 8 周路线

| 周 | 主题 | 阅读 | 动手验收 |
|---|---|---|---|
| 1 | 认识板子 | `/proc`、`/sys`、`/dev` 目录 | 用 `cat` 找到：CPU 温度、内存、eMMC 大小、按键 event 号 |
| 2 | Shell | `external/rkscript/S00mountall.sh`、`external/rockchip-test/video/video_test.sh` | 写脚本：遍历 `/sys/class/thermal`，打印所有温度 |
| 3 | 文件 I/O | `01_read_sysfs.c`、`echo_emmc_test.c` | 不看示例，独立写出"读任意 sysfs 文件并解析数字" |
| 4 | 标准 IO/进程 | `05_popen_meminfo.c`、`echo_ddr_test.c` | 用 popen 解析 `/proc/stat`，输出 CPU 占用率 |
| 5 | 设备与 ioctl | `03_read_input.c`、`06_ioctl_vendor.c` | 写一个"按键计数 + 长按检测"程序 |
| 6 | mmap/显示 | `04_mmap_copy.c`、`uvc_app/main.c` | 用 mmap 写 `/dev/fb0` 画彩色条纹（无 fb0 则用 DRM） |
| 7 | 进程/线程/网络 | `external/rk_pcba_test/echo_pcbatest_server.c` | 写 TCP 服务端：板子每秒上报一次温度 |
| 8 | 构建系统 | `buildroot/package/rockchip/rktoolkit/*.mk` | 把自己的小程序做成 Buildroot 包并编进镜像 |

## 5. 板端常用排查命令（背下来）

```bash
cat /proc/cpuinfo                 # CPU 信息
cat /proc/meminfo                 # 内存
cat /proc/interrupts              # 中断
cat /proc/bus/input/devices       # 输入设备与 event 号
ls /sys/class/thermal             # 温度传感器
ls /sys/class/leds                # LED
ls /sys/class/gpio                # GPIO（旧接口）
ls /dev | head -50                # 设备节点
dmesg | tail -50                  # 内核日志
cat /proc/device-tree/model       # 板型
mount                             # 挂载情况
df -h                             # 空间
```

调试组合拳：

```bash
strace -f -e trace=openat,read,write,ioctl ./你的程序   # 看系统调用
ldd ./你的程序                                          # 看依赖库
file ./你的程序                                         # 看架构
readelf -h ./你的程序                                   # ELF 头
printf '1' > /sys/class/leds/led1/brightness            # 不用 echo（避免换行）
```

## 6. 新手最容易踩的坑

1. **Exec format error**：编译架构不对，`file` 确认是 `ARM aarch64`。
2. **error while loading shared libraries**：动态库没带上；初学一律加 `-static`。
3. **sysfs 写不进去**：权限不足 / 值不合法 / 一次 write 写了多次。
4. **读 sysfs 得到乱码或 0**：read 不会自动补 `'\0'`，且内容可能是 `0x0a` 结尾。
5. **结构体对不上**：ioctl 的结构体必须和内核头文件完全一致（字段顺序、宽度、对齐）。
6. **不要写 `/dev/mmcblk*`、`/dev/mtd*`、`/dev/vendor_storage` 的 SN/MAC**，
   写坏分区或校准数据只能返厂/重刷。
7. **文件描述符泄漏**：每个 `open` 都要有 `close`，`mmap` 要有 `munmap`。
8. **只读根文件系统**：写文件用 `/userdata`、`/data`、`/tmp`。
9. **中文注释乱码**：SDK 里大量 GBK 文件，用 `iconv -f GB18030 -t UTF-8` 转。
10. **Ctrl+C 后设备没释放**：程序里加 `signal(SIGINT, ...)` 做清理，或确认 fd 已关闭。

---

## 7. 下一步

- 想深入"设备驱动"：读 `kernel/drivers/soc/rockchip/rk_vendor_storage.c`，
  对照 `06_ioctl_vendor.c` 看 ioctl 两边如何对应。
- 想深入"多媒体"：`external/uvc_app`（UVC gadget）→ `external/mpp/test`（编解码）。
- 想深入"构建"：`buildroot/package/rockchip/` 下任选一个包，照抄写自己的包。
