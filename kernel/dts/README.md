# 100ASK i.MX6ULL：ICM20602 与 SSD1306 设备树适配

本目录的 `imx6ull-imu-oled.dtsi` 是对下列厂商 Linux 4.9.88 板级文件的增量片段：

- `arch/arm/boot/dts/100ask_imx6ull_mini.dts`
- `arch/arm/boot/dts/100ask_imx6ull-14x14.dts`

核查来源是远端 WSL 导出备份中的
`/home/yan/workspace/100ask_imx6ull-sdk/Linux-4.9.88`。这里只能证明源码中的复用和占用关系；没有开发板原理图、扩展口定义和实物连线，不能证明候选 GPIO 已引到可用排针，也不能把编译通过当作硬件验证通过。

## 1. 厂商 DTS 核查结果

两份板级 DTS 在本项目所需资源上的定义相同：

| 资源 | 厂商 DTS 中的状态 | 本项目选择 |
| --- | --- | --- |
| ECSPI1 | 已启用；SCLK=`CSI_DATA04`，MOSI=`CSI_DATA06`，MISO=`CSI_DATA07`；GPIO CS0=`CSI_DATA05/GPIO4_IO26`，CS1=`CSI_DATA03/GPIO4_IO24`；两个示例 spidev 节点均被注释 | 使用 CS0，添加 `imu@0` |
| ECSPI3 | 已启用；CS=`GPIO1_IO20`，已有 `icm20608@0`，IRQ=`GPIO1_IO01` | 不覆盖、不复用 |
| ECSPI2/ECSPI4 | 板级 DTS 未启用且没有板级 pinctrl | 不选用 |
| 根节点 `spi4` | 这是供 74HC595 使用的 `spi-gpio`，不是 SoC ECSPI4 | 不选用 |
| I²C1 | 已启用；SCL=`UART4_TX_DATA`，SDA=`UART4_RX_DATA`，没有活动子设备 | 添加 `oled@3c` |
| I²C2 | 已启用；已有 WM8960@0x1a、SII902x@0x39、GT9xx@0x5d | 不选用 |

`imx6ull.dtsi` 的 alias 是 `spi0 = &ecspi1`、`i2c0 = &i2c1`。因此在当前厂商设备树下，预期枚举名分别是 `spi0.0` 和 I²C adapter 0；运行时仍应以 `/sys/bus/spi/devices` 与 `i2cdetect -l` 的实际结果为准，不要仅按控制器后缀猜 `/dev/i2c-*` 编号。

## 2. 候选接线

下表使用 SoC pad 名，而不是开发板排针号。必须先用对应板型的原理图/排针图把 pad 映射成实际针脚。

| 模块信号 | i.MX6ULL 信号/pad | 说明 |
| --- | --- | --- |
| ICM20602 SCLK | ECSPI1_SCLK / `CSI_DATA04` | 厂商现有 pinctrl |
| ICM20602 SDI/MOSI | ECSPI1_MOSI / `CSI_DATA06` | 厂商现有 pinctrl |
| ICM20602 SDO/MISO | ECSPI1_MISO / `CSI_DATA07` | 厂商现有 pinctrl |
| ICM20602 nCS | GPIO4_IO26 / `CSI_DATA05` | ECSPI1 CS0，低有效 |
| ICM20602 INT/DRDY | GPIO4_IO23 / `CSI_DATA02` | 本片段新增候选；两份 DTS 中未发现占用，但是否引出必须确认 |
| SSD1306 SCL | I2C1_SCL / `UART4_TX_DATA` | 厂商现有 pinctrl |
| SSD1306 SDA | I2C1_SDA / `UART4_RX_DATA` | 厂商现有 pinctrl |
| 两模块 VCC/GND | 3.3 V / GND | 共地；不要让 I²C 上拉到 5 V |

还需现场确认：

1. `CSI_DATA02/GPIO4_IO23` 是否确实引到当前板型的可用排针，且没有被板外电路占用。若不是，应同时修改 `MX6UL_PAD_*__GPIO*`、`interrupt-parent` 和 `interrupts` 三处。
2. ICM20602 驱动把 INT 配成高有效 Data Ready；本片段据此使用 `IRQ_TYPE_EDGE_RISING`。若实际模块带反相器或驱动改成低有效，必须同步修改触发类型。
3. OLED 模块地址脚配置后是否为 `0x3c`；部分模块可能是 `0x3d`。
4. SDA/SCL 是否已有合适的 3.3 V 上拉。很多 OLED 模块自带上拉，不能仅凭模块外观判断。
5. ECSPI1 CS0 与 I²C1 信号在所用扩展口是否同时可达。DTS 只证明 SoC 复用关系，不证明连接器布线。

## 3. 合入厂商内核

在修改厂商源码前，可以先从项目根目录运行两个板型的合并语法检查：

```bash
make dtb-check
```

该目标通过 wrapper 将原板级 DTS 与增量片段合并，并使用厂商内核自带 `dtc` 生成测试 DTB；它不会替换开发板正在启动的 DTB。

将片段复制到内核设备树目录：

```bash
cp kernel/dts/imx6ull-imu-oled.dtsi \
  /home/yan/workspace/100ask_imx6ull-sdk/Linux-4.9.88/arch/arm/boot/dts/
```

在实际启动所用的板级 DTS 最后一行之后添加 include。以 mini 板为例：

```dts
#include "imx6ull-imu-oled.dtsi"
```

该 include 建议放在板级 DTS 末尾，便于审阅并确保它是板级资源定义之后的增量。不要同时在 ECSPI1 CS0 下保留另一个活动的 `reg = <0>` 子节点。

按实际板型只编译对应 DTB；交叉工具链前缀以当前 SDK 环境为准：

```bash
cd /home/yan/workspace/100ask_imx6ull-sdk/Linux-4.9.88
make ARCH=arm \
  CROSS_COMPILE=/home/yan/workspace/100ask_imx6ull-sdk/ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf- \
  100ask_imx6ull_mini.dtb
# 或：
make ARCH=arm \
  CROSS_COMPILE=/home/yan/workspace/100ask_imx6ull-sdk/ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf- \
  100ask_imx6ull-14x14.dtb
```

替换开发板上的 DTB 前先保留原文件，并确认 U-Boot 实际加载的是哪个文件名。仅复制新 DTB 不代表 U-Boot 已选择它。

## 4. 板上核查

启动后先核对设备树和总线枚举，再加载模块：

```bash
ls /sys/bus/spi/devices
i2cdetect -l
cat /proc/interrupts
cat /sys/kernel/debug/gpio
cat /sys/kernel/debug/pinctrl/*/pinmux-pins
```

预期 ECSPI1/CS0 对应的 SPI 设备出现，SSD1306 驱动绑定前地址扫描通常可见 `0x3c`，绑定后可能显示 `UU`。如果 IMU 能 probe 但 `/proc/interrupts` 计数不增长，优先检查 INT 电平、触发沿、GPIO pad 到排针的映射以及模块是否真正输出 Data Ready。
