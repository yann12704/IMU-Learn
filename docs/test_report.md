# 测试报告

## 测试分级

- **已验证**：在本机导入的 Ubuntu20-iMX6ULL 和厂商 SDK 中实际执行成功。
- **静态检查**：已检查源码或产物存在，但没有连接目标硬件。
- **待板上验证**：必须由 i.MX6ULL、IMU、OLED 和实际接线完成。

## 本机环境

```text
WSL distribution : Ubuntu20-iMX6ULL (WSL 2)
Userspace         : Ubuntu 20.04.6 LTS
Kernel source     : 100ASK Linux 4.9.88
Toolchain         : Linaro GCC 6.2.1 arm-linux-gnueabihf
```

## 当前结果

| 项目 | 状态 | 结果 |
|---|---|---|
| WSL 启动 | 已验证 | 默认用户 `yan`，发行版可正常执行命令 |
| 厂商内核构建系统 | 已验证 | `make ... kernelrelease` 返回 `4.9.88` |
| 工具链执行 | 已验证 | Linaro GCC 6.2.1 正常运行 |
| 用户态应用交叉编译 | 需重跑 | 初版为 ELF32 ARM EABI5；最终版新增静止校验、Euler 速率与动态 accel 门控后受执行授权额度限制 |
| Buildroot 动态加载器 | 静态检查 | rootfs 含 `/lib/ld-linux-armhf.so.3`；仍需核对全部 GLIBC 符号或在板上执行 |
| IMU 模块编译 | 需重跑 | 初版曾通过 4.9.88 构建；最终版修正原厂寄存器与 kref 生命周期后受执行授权额度限制，旧 `.ko` 不可交付 |
| OLED 模块编译 | 需重跑 | 初版曾通过 4.9.88 构建；最终 kref 源码晚于旧 `.ko`，旧产物不可交付 |
| 设备树预处理 | 需重跑 | 初版 mini DTS 合并成功；最终片段和新增 `dtb-check` 目标需再次执行 |
| 最终 DTB 编译 | 未完成 | 厂商 `dtc` 调用被本会话系统执行授权额度阻止；不是 DTS 编译诊断 |
| ICM20602 WHO_AM_I | 待板上验证 | 需要真实 SPI 硬件 |
| Data Ready IRQ | 待板上验证 | 需要真实 GPIO 和 `/proc/interrupts` |
| IIO raw/scale | 待板上验证 | 需要驱动成功 probe |
| OLED 字符显示 | 待板上验证 | 需要真实 SSD1306 |
| 姿态滤波动态效果 | 待板上验证 | 需要采集数据和标定 |

## 板上验收指标

1. WHO_AM_I 稳定返回 `0x12`。
2. 100 Hz 配置下，60 秒 IRQ/sample 计数误差在可解释范围内。
3. 正常运行时 ring buffer 不持续丢样。
4. 静止放置时 roll/pitch 抖动和 bias 有实测记录。
5. OLED 约 10 Hz 刷新，不阻塞 100 Hz 姿态计算。
6. 保存串口日志、debugfs 统计和测试条件，避免只保留结论。
