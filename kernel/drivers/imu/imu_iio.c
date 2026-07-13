// SPDX-License-Identifier: GPL-2.0
/* IIO direct-mode interface for the ICM20602 accelerometer and gyroscope. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "imu_core.h"

enum imu_iio_address {
	IMU_ADDR_ACCEL_X,
	IMU_ADDR_ACCEL_Y,
	IMU_ADDR_ACCEL_Z,
	IMU_ADDR_GYRO_X,
	IMU_ADDR_GYRO_Y,
	IMU_ADDR_GYRO_Z,
};

#define IMU_IIO_CHANNEL(_type, _modifier, _address) { \
	.type = (_type), \
	.modified = 1, \
	.channel2 = (_modifier), \
	.address = (_address), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	.scan_index = -1, \
}

static const struct iio_chan_spec imu_iio_channels[] = {
	IMU_IIO_CHANNEL(IIO_ACCEL, IIO_MOD_X, IMU_ADDR_ACCEL_X),
	IMU_IIO_CHANNEL(IIO_ACCEL, IIO_MOD_Y, IMU_ADDR_ACCEL_Y),
	IMU_IIO_CHANNEL(IIO_ACCEL, IIO_MOD_Z, IMU_ADDR_ACCEL_Z),
	IMU_IIO_CHANNEL(IIO_ANGL_VEL, IIO_MOD_X, IMU_ADDR_GYRO_X),
	IMU_IIO_CHANNEL(IIO_ANGL_VEL, IIO_MOD_Y, IMU_ADDR_GYRO_Y),
	IMU_IIO_CHANNEL(IIO_ANGL_VEL, IIO_MOD_Z, IMU_ADDR_GYRO_Z),
};

static int imu_iio_sample_value(const struct imu_sample *sample,
				unsigned long address, int *val)
{
	switch (address) {
	case IMU_ADDR_ACCEL_X:
		*val = sample->ax;
		break;
	case IMU_ADDR_ACCEL_Y:
		*val = sample->ay;
		break;
	case IMU_ADDR_ACCEL_Z:
		*val = sample->az;
		break;
	case IMU_ADDR_GYRO_X:
		*val = sample->gx;
		break;
	case IMU_ADDR_GYRO_Y:
		*val = sample->gy;
		break;
	case IMU_ADDR_GYRO_Z:
		*val = sample->gz;
		break;
	default:
		return -EINVAL;
	}

	return IIO_VAL_INT;
}

static int imu_iio_read_raw(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan,
			    int *val, int *val2, long mask)
{
	struct imu_device *imu = imu_from_iio(indio_dev);
	struct imu_sample sample;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = imu_read_sample(imu, &sample);
		if (ret)
			return ret;
		return imu_iio_sample_value(&sample, chan->address, val);

	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		if (chan->type == IIO_ACCEL) {
			/* +/-2 g: 9.80665 / 16384 m/s^2 per LSB. */
			*val2 = 598550;
			return IIO_VAL_INT_PLUS_NANO;
		}
		if (chan->type == IIO_ANGL_VEL) {
			/* +/-250 dps (131 LSB/dps), expressed as rad/s per LSB. */
			*val2 = 133158;
			return IIO_VAL_INT_PLUS_NANO;
		}
		return -EINVAL;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = READ_ONCE(imu->sampling_frequency);
		*val2 = 0;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int imu_iio_write_raw(struct iio_dev *indio_dev,
			     const struct iio_chan_spec *chan,
			     int val, int val2, long mask)
{
	struct imu_device *imu = imu_from_iio(indio_dev);
	unsigned int actual;

	(void)chan;
	if (mask != IIO_CHAN_INFO_SAMP_FREQ || val <= 0 || val2 != 0)
		return -EINVAL;

	return imu_set_sampling_frequency(imu, val, &actual);
}

static int imu_iio_write_raw_get_fmt(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     long mask)
{
	(void)indio_dev;
	(void)chan;
	if (mask == IIO_CHAN_INFO_SAMP_FREQ)
		return IIO_VAL_INT;

	return -EINVAL;
}

static const struct iio_info imu_iio_info = {
	.driver_module = THIS_MODULE,
	.read_raw = imu_iio_read_raw,
	.write_raw = imu_iio_write_raw,
	.write_raw_get_fmt = imu_iio_write_raw_get_fmt,
};

int imu_iio_register(struct imu_device *imu)
{
	struct iio_dev *indio_dev = imu->indio_dev;

	indio_dev->dev.parent = &imu->spi->dev;
	indio_dev->name = "icm20602";
	indio_dev->info = &imu_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = imu_iio_channels;
	indio_dev->num_channels = ARRAY_SIZE(imu_iio_channels);

	return iio_device_register(indio_dev);
}

void imu_iio_unregister(struct imu_device *imu)
{
	iio_device_unregister(imu->indio_dev);
}
