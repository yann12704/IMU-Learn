/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef IMX6ULL_IMU_SAMPLE_H
#define IMX6ULL_IMU_SAMPLE_H

#include <linux/types.h>

#define IMU_SAMPLE_ABI_VERSION 1U
#define IMU_SAMPLE_WIRE_SIZE   24U

/*
 * Native-endian ABI shared by the i.MX6ULL kernel driver and its userspace
 * application. reserved0 makes the 64-bit timestamp alignment explicit on
 * both 32-bit ARM and 64-bit build hosts.
 */
struct imu_sample {
	__s16 ax;
	__s16 ay;
	__s16 az;
	__s16 gx;
	__s16 gy;
	__s16 gz;
	__u32 reserved0;
	__u64 timestamp_ns;
};

#endif /* IMX6ULL_IMU_SAMPLE_H */
