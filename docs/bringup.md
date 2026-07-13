# 开发板 Bring-up 指南

## 1. 构建

进入迁移后的构建环境：

```powershell
wsl -d Ubuntu20-iMX6ULL
```

```bash
cd /mnt/e/ws/imx6ull-imu-attitude
make all
```

确认用户态程序架构和模块信息：

```bash
file app/attitude_app
modinfo kernel/drivers/imu/*.ko
modinfo kernel/drivers/oled/*.ko
```

## 2. 设备树

1. 打开实际使用的 100ASK 板级 DTS。
2. 根据原理图核对 ECSPI 控制器、CS GPIO、DRDY GPIO、I2C 控制器和 OLED 地址。
3. 将 `kernel/dts/imx6ull-imu-oled.dtsi` 中需要的节点合并到板级 DTS。
4. 检查同一 pin 是否已经被 LCD、摄像头、UART、按键或其他外设占用。
5. 编译对应 DTB，不要只编译一个未被 U-Boot 加载的文件。

部署后先确认 live device tree：

```bash
find /proc/device-tree -name '*imu*' -o -name '*oled*'
```

## 3. 总线预检查

加载自定义驱动前可先检查控制器是否存在：

```bash
ls /sys/bus/spi/devices
i2cdetect -l
# 当前厂商 DTS 中 i2c0 alias 指向 SoC I2C1，通常随后检查：
i2cdetect -y 0
cat /sys/kernel/debug/gpio
cat /sys/kernel/debug/pinctrl/*/pinmux-pins
```

不要在自定义驱动已经绑定 OLED 时继续用 `i2cdetect`/`i2cset` 修改同一设备。

## 4. 模块加载与 probe

将 `.ko` 放到开发板后，用 `insmod` 加载：

```bash
insmod ./kernel/drivers/imu/icm20602_imu.ko
insmod ./kernel/drivers/oled/ssd1306_i2c.ko
dmesg | tail -n 100
```

期望结果：

```bash
ls -l /dev/imu0 /dev/oled0
ls /sys/bus/iio/devices
cat /proc/interrupts | grep -E 'spi0\.0|icm20602'
```

若 IMU 报 WHO_AM_I 错误，依次检查：供电、电平、SPI mode、CS、MISO/MOSI 是否接反以及最高频率。不要通过跳过 ID 校验掩盖硬件问题。

## 5. 数据流验证

观察 IRQ 是否增长：

```bash
watch -n 1 "cat /proc/interrupts | grep -E 'spi0\\.0|icm20602'"
```

读取一个 IIO raw 属性：

```bash
grep -R . /sys/bus/iio/devices/iio:device*/name
cat /sys/bus/iio/devices/iio:device*/in_accel_x_raw
```

查看驱动统计（具体目录名以实现为准）：

```bash
cat /sys/kernel/debug/imu0/stats
```

统计中的 `irq_count` 与 `sample_count` 应持续增加；`drop_count` 长期增长说明用户态读取不及时。

## 6. OLED 验证

```bash
printf 'R:  1.23\nP: -2.34\nY:  0.00\n' > /dev/oled0
```

若无显示但 probe 成功，检查地址、供电、屏幕分辨率、COM 扫描方向和模块是否实际为 SH1106；SH1106 与 SSD1306 的列寻址并不完全相同。

## 7. 姿态程序

开发板保持静止后运行：

```bash
chmod +x attitude_app
./attitude_app -n 200 -a 0.98
```

当前应用直接使用字符 ABI 中的传感器 XYZ，没有实现 mounting matrix。若模块安装方向与期望机体坐标系不一致，应在应用中增加明确的 signed-permutation 轴映射；IIO mounting matrix 不会自动改变 `/dev/imu0` 数据。不能只在最终显示时随意取反。

## 8. 卸载

先退出用户态程序，再卸载模块：

```bash
rmmod ssd1306_i2c
rmmod icm20602_imu
```

若设备文件仍被打开，模块引用计数会阻止卸载，这是预期保护行为。
