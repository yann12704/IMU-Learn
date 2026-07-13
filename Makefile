SDK_ROOT ?= /home/yan/workspace/100ask_imx6ull-sdk
KERNEL_DIR ?= $(SDK_ROOT)/Linux-4.9.88
TOOLCHAIN_BIN ?= $(SDK_ROOT)/ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf/bin
CROSS_COMPILE ?= $(TOOLCHAIN_BIN)/arm-linux-gnueabihf-
ARCH ?= arm
HOSTCC ?= gcc

IMU_MODULE_DIR := $(CURDIR)/kernel/drivers/imu
OLED_MODULE_DIR := $(CURDIR)/kernel/drivers/oled
DTS_DIR := $(CURDIR)/kernel/dts
DTS_TEST_DIR := $(DTS_DIR)/tests
DTC := $(KERNEL_DIR)/scripts/dtc/dtc
DTS_CPPFLAGS := -nostdinc -I$(KERNEL_DIR)/arch/arm/boot/dts \
	-I$(KERNEL_DIR)/include -I$(DTS_DIR) -undef -D__DTS__ \
	-x assembler-with-cpp

.PHONY: all modules imu-module oled-module app dtb-check clean

all: modules app dtb-check

modules: imu-module oled-module

imu-module:
	$(MAKE) -C $(KERNEL_DIR) M=$(IMU_MODULE_DIR) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

oled-module:
	$(MAKE) -C $(KERNEL_DIR) M=$(OLED_MODULE_DIR) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

app:
	$(MAKE) -C app CROSS_COMPILE=$(CROSS_COMPILE)

dtb-check: $(DTS_TEST_DIR)/mini.dtb $(DTS_TEST_DIR)/14x14.dtb

$(DTS_TEST_DIR)/mini.pp.dts: \
		$(DTS_TEST_DIR)/100ask_imx6ull_mini_with_imu.dts \
		$(DTS_DIR)/imx6ull-imu-oled.dtsi
	$(HOSTCC) -E $(DTS_CPPFLAGS) -o $@ $<

$(DTS_TEST_DIR)/14x14.pp.dts: \
		$(DTS_TEST_DIR)/100ask_imx6ull_14x14_with_imu.dts \
		$(DTS_DIR)/imx6ull-imu-oled.dtsi
	$(HOSTCC) -E $(DTS_CPPFLAGS) -o $@ $<

$(DTS_TEST_DIR)/mini.dtb: $(DTS_TEST_DIR)/mini.pp.dts
	$(DTC) -I dts -O dtb -o $@ $<

$(DTS_TEST_DIR)/14x14.dtb: $(DTS_TEST_DIR)/14x14.pp.dts
	$(DTC) -I dts -O dtb -o $@ $<

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(IMU_MODULE_DIR) clean
	$(MAKE) -C $(KERNEL_DIR) M=$(OLED_MODULE_DIR) clean
	$(MAKE) -C app clean
	$(RM) $(DTS_TEST_DIR)/mini.pp.dts $(DTS_TEST_DIR)/mini.dtb
	$(RM) $(DTS_TEST_DIR)/14x14.pp.dts $(DTS_TEST_DIR)/14x14.dtb
