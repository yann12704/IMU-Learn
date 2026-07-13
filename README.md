# i.MX6ULL IMU 姿态监测系统

面向 100ASK i.MX6ULL / Linux 4.9.88 的教学型完整链路实现：ICM20602 SPI 驱动采集六轴原始数据，内核通过字符设备和 IIO 暴露数据，用户态执行互补滤波，并通过 SSD1306 I2C 驱动显示姿态角。

## 功能范围

- ICM20602 SPI 寄存器访问、WHO_AM_I 校验和初始化
- Data Ready threaded IRQ 采样
- ring buffer、wait queue、阻塞/非阻塞 `read()`、`poll()`
- `/dev/imu0` 固定 ABI 数据流
- IIO accel/gyro raw、scale、sampling frequency
- debugfs 运行统计
- SSD1306 128x64 framebuffer、5x7 字体和 `/dev/oled0`
- 用户态静止零偏标定、roll/pitch 互补滤波和相对 yaw 积分

## 硬件假设

- 开发板：100ASK i.MX6ULL
- IMU：ICM20602，SPI mode 0，WHO_AM_I 为 `0x12`
- OLED：SSD1306，128x64，I2C 地址 `0x3c`
- 默认采样/显示频率：100 Hz / 10 Hz

实际 SPI 控制器、片选、Data Ready GPIO 和 I2C 引脚必须按开发板原理图与接线核对。设备树片段中的注释是部署前检查项，不应直接忽略。

## 构建环境

本项目默认使用已导入的 WSL：

```powershell
wsl -d Ubuntu20-iMX6ULL
```

WSL 内执行：

```bash
cd /mnt/e/ws/imx6ull-imu-attitude
make all
```

> 当前目录可能残留首次审查前生成的对象或二进制；源码才是基准。正式部署前应执行 `make clean && make all`，并确认时间戳和 `vermagic` 对应最终源码。不要直接使用未经最终重编的旧 `.ko`。

默认路径可在命令行覆盖：

```bash
make all \
  KERNEL_DIR=/path/to/Linux-4.9.88 \
  CROSS_COMPILE=/path/to/arm-linux-gnueabihf-
```

单独构建：

```bash
make imu-module
make oled-module
make app
```

## 部署概要

1. 按 `kernel/dts/imx6ull-imu-oled.dtsi` 的注释核对接线和已有 pinctrl 占用。
2. 将节点合入实际板级 DTS，重新编译并部署 DTB。
3. 将两个 `.ko` 和 `app/attitude_app` 复制到开发板。
4. 加载驱动后确认 `/dev/imu0`、`/dev/oled0` 和 IIO 设备出现。
5. 保持开发板静止，启动应用完成陀螺仪零偏估计。

```bash
./attitude_app -i /dev/imu0 -o /dev/oled0 -n 200 -a 0.98
```

没有 OLED 时：

```bash
./attitude_app --no-oled
```

完整步骤见 [docs/bringup.md](docs/bringup.md)。

## 数据 ABI

`include/imu_sample.h` 同时供内核和用户态使用。每个样本固定 24 字节：

```text
ax ay az gx gy gz : 6 x signed 16-bit raw value
reserved0         : 32-bit, must be zero
timestamp_ns      : 64-bit monotonic timestamp
```

默认配置下，加速度换算为 `raw / 16384 g`，角速度换算为 `raw / 131 dps`。

## 重要限制

- ICM20602 没有磁力计，yaw 是相对角度并会随时间漂移。
- 当前 IIO 实现目标是 direct mode；连续流由 `/dev/imu0` 提供。
- 设备树没有提供有效 Data Ready IRQ 时，只注册 IIO direct mode，不创建 `/dev/imu0`。
- 当前应用直接使用传感器原始坐标轴；安装方向不一致时必须增加明确的轴交换/符号映射。
- 未接开发板时只能证明源码和交叉构建通过，不能证明 SPI 时序、IRQ 极性或 OLED 实物显示正确。
- 驱动代码面向厂商 Linux 4.9.88，不保证可直接在新内核编译。

设计与验证记录见：

- [docs/driver_design.md](docs/driver_design.md)
- [docs/test_report.md](docs/test_report.md)
- [项目执行计划](../plan.md)
