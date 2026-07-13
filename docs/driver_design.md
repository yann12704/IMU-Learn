# 驱动设计说明

## 1. 数据路径

```text
ICM20602 DRDY GPIO
  -> hard IRQ（只返回 IRQ_WAKE_THREAD）
  -> threaded IRQ（SPI burst read）
  -> imu_sample + monotonic timestamp
  -> ring buffer
  -> wake_up_interruptible()
  -> /dev/imu0 read()/poll()
  -> attitude_app
```

IIO direct mode 与中断采样共享设备 mutex，避免两条 SPI 事务交错。IIO `read_raw` 用于调试和标准属性访问，字符设备用于连续样本流。

## 2. ring buffer 策略

ring buffer 容量固定为 2 的幂。生产者是 threaded IRQ，消费者是用户进程：

- 索引和样本复制由 spinlock 保护；
- 无数据时阻塞读者，`O_NONBLOCK` 返回 `-EAGAIN`；
- 新样本入队后唤醒 wait queue；
- 满队列覆盖最旧样本并递增 drop 统计，使姿态应用优先获得最新状态；
- `poll()` 在队列非空时返回 `POLLIN | POLLRDNORM`。

## 3. 锁与上下文

- SPI 访问可能睡眠，只能放在进程上下文或 threaded IRQ 中。
- `xfer_lock` mutex 串行化寄存器访问、IIO 直接读取和采样率修改。
- ring lock 只保护内存队列，不在持锁期间执行 SPI 或 `copy_to_user()`。
- 统计使用原子计数或在对应锁保护下更新。

## 4. 字符 ABI

字符设备一次返回一个或多个完整 `struct imu_sample`。驱动拒绝小于单样本大小的读取，且永不返回半个样本。结构包含显式保留字段，确保 ARM 32 位内核和用户态布局一致。

## 5. ICM20602 配置

默认目标：

- 时钟：PLL
- 加速度量程：±2 g
- 陀螺仪量程：±250 dps
- 数字低通开启
- 内部采样基准：1 kHz
- 输出采样：100 Hz
- Data Ready 中断：每个新样本触发

初始化严格按 [TDK InvenSense ICM-20602 DS-000176 v1.1](https://product.tdk.com/system/files/dam/doc/product/sensor/mortion-inertial/imu/data_sheet/ds-000176-icm-20602-v1.1.pdf)：复位等待后写 `I2C_IF(0x70).I2C_IF_DIS`，每次上电设置 `ACCEL_INTEL_CTRL(0x69).OUTPUT_LIMIT`，退出 sleep 后等待最坏 100 ms gyro drive-start 时间。不要套用 MPU-60x0 的 `USER_CTRL(0x6a).bit4`。

驱动使用从 `ACCEL_XOUT_H` 开始的一次 burst read，取出 accel XYZ 和 gyro XYZ；温度寄存器在 burst 中跳过。

## 6. IIO 语义

- `IIO_ACCEL` raw 是传感器原始 signed 16-bit 值；scale 使用 m/s² 每 LSB。
- `IIO_ANGL_VEL` raw 是原始 signed 16-bit 值；scale 使用 rad/s 每 LSB。
- `sampling_frequency` 返回驱动当前配置的实际频率。

## 7. OLED 写入模型

`/dev/oled0` 的每次 `write()` 被视为一帧文本：

1. 限长复制用户输入；
2. 清空 framebuffer；
3. 处理可打印 ASCII 和换行；
4. 按页分块发送 1024 字节 framebuffer。

整个过程由 mutex 串行化。显示刷新只由用户态以约 10 Hz 触发，不跟随每个 100 Hz IMU 样本刷新。

## 8. 姿态算法边界

roll/pitch 的短时变化来自 gyro 积分，长期趋势由重力方向校正。滤波器只在加速度模长接近 1 g 时采用重力观测，并用最短角差处理 ±180° 跨越。gyro 机体系角速度通过 3-2-1 Euler 速率关系更新姿态，在 pitch 接近 ±90° 时仍存在表示奇异性。yaw 没有磁力计观测；静止标定不能消除温漂，因此长期漂移是预期行为。
