// SPDX-License-Identifier: GPL-2.0
/*
 * ICM20602 SPI IMU driver for the 100ASK i.MX6ULL Linux 4.9 BSP.
 *
 * Data-ready interrupts are handled by a threaded handler because every
 * sample requires a sleeping SPI transfer.  The same transfer lock is used by
 * the IIO direct-mode path, so sysfs reads cannot interleave with an IRQ burst.
 */

#include <linux/bug.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <asm/unaligned.h>

#include "imu_core.h"

#define ICM20602_REG_SMPLRT_DIV		0x19
#define ICM20602_REG_CONFIG		0x1a
#define ICM20602_REG_GYRO_CONFIG	0x1b
#define ICM20602_REG_ACCEL_CONFIG	0x1c
#define ICM20602_REG_ACCEL_CONFIG2	0x1d
#define ICM20602_REG_INT_PIN_CFG		0x37
#define ICM20602_REG_INT_ENABLE		0x38
#define ICM20602_REG_ACCEL_XOUT_H	0x3b
#define ICM20602_REG_ACCEL_INTEL_CTRL	0x69
#define ICM20602_REG_PWR_MGMT_1		0x6b
#define ICM20602_REG_PWR_MGMT_2		0x6c
#define ICM20602_REG_I2C_IF		0x70
#define ICM20602_REG_WHO_AM_I		0x75

#define ICM20602_SPI_READ		BIT(7)
#define ICM20602_DEVICE_RESET		BIT(7)
#define ICM20602_SLEEP			BIT(6)
#define ICM20602_CLK_PLL_XGYRO		0x01
#define ICM20602_I2C_IF_DIS		BIT(6)
#define ICM20602_INT_ANYRD_2CLEAR	BIT(4)
#define ICM20602_DATA_RDY_INT		BIT(0)
#define ICM20602_ACCEL_OUTPUT_LIMIT	BIT(1)

#define ICM20602_GYRO_FS_250DPS		0x00
#define ICM20602_ACCEL_FS_2G		0x00
#define ICM20602_DLPF_20HZ		0x04

#define ICM20602_BURST_SIZE		14U
#define ICM20602_INTERNAL_RATE_HZ	1000U
#define ICM20602_MIN_RATE_HZ		4U
#define ICM20602_MAX_RATE_HZ		1000U
#define ICM20602_MAX_SPI_HZ		10000000U

static void imu_device_release(struct kref *ref)
{
	struct imu_device *imu = container_of(ref, struct imu_device, refcount);

	mutex_destroy(&imu->lifecycle_lock);
	mutex_destroy(&imu->xfer_lock);
	kfree(imu);
}

void imu_device_get(struct imu_device *imu)
{
	kref_get(&imu->refcount);
}

void imu_device_put(struct imu_device *imu)
{
	kref_put(&imu->refcount, imu_device_release);
}

static int imu_spi_write_reg(struct imu_device *imu, u8 reg, u8 value)
{
	u8 tx[2] = { reg & ~ICM20602_SPI_READ, value };

	return spi_write(imu->spi, tx, sizeof(tx));
}

static int imu_spi_read_reg(struct imu_device *imu, u8 reg, u8 *value)
{
	u8 command = reg | ICM20602_SPI_READ;

	return spi_write_then_read(imu->spi, &command, sizeof(command), value, 1);
}

static int imu_read_sample_locked(struct imu_device *imu,
				  struct imu_sample *sample)
{
	u8 command = ICM20602_REG_ACCEL_XOUT_H | ICM20602_SPI_READ;
	u8 data[ICM20602_BURST_SIZE];
	u64 start_ns;
	u64 elapsed_ns;
	int ret;

	start_ns = ktime_get_ns();
	ret = spi_write_then_read(imu->spi, &command, sizeof(command), data,
				  sizeof(data));
	elapsed_ns = ktime_get_ns() - start_ns;
	atomic64_set(&imu->last_spi_read_ns, elapsed_ns);
	if (ret)
		return ret;

	/* The two temperature bytes at offsets 6 and 7 are intentionally skipped. */
	sample->ax = (s16)get_unaligned_be16(&data[0]);
	sample->ay = (s16)get_unaligned_be16(&data[2]);
	sample->az = (s16)get_unaligned_be16(&data[4]);
	sample->gx = (s16)get_unaligned_be16(&data[8]);
	sample->gy = (s16)get_unaligned_be16(&data[10]);
	sample->gz = (s16)get_unaligned_be16(&data[12]);
	sample->reserved0 = 0;
	sample->timestamp_ns = ktime_get_ns();
	atomic64_inc(&imu->sample_count);

	return 0;
}

int imu_read_sample(struct imu_device *imu, struct imu_sample *sample)
{
	int ret;

	if (READ_ONCE(imu->removing))
		return -ENODEV;

	mutex_lock(&imu->xfer_lock);
	if (imu->removing)
		ret = -ENODEV;
	else
		ret = imu_read_sample_locked(imu, sample);
	mutex_unlock(&imu->xfer_lock);

	atomic_set(&imu->last_error, ret);
	return ret;
}

int imu_set_sampling_frequency(struct imu_device *imu, unsigned int requested,
			       unsigned int *actual)
{
	unsigned int divider;
	unsigned int programmed_rate;
	int ret;

	if (requested < ICM20602_MIN_RATE_HZ ||
	    requested > ICM20602_MAX_RATE_HZ)
		return -EINVAL;

	divider = DIV_ROUND_CLOSEST(ICM20602_INTERNAL_RATE_HZ, requested);
	divider = clamp_t(unsigned int, divider, 1U, 256U);
	programmed_rate = ICM20602_INTERNAL_RATE_HZ / divider;

	mutex_lock(&imu->xfer_lock);
	if (imu->removing) {
		ret = -ENODEV;
	} else {
		ret = imu_spi_write_reg(imu, ICM20602_REG_SMPLRT_DIV,
					(u8)(divider - 1U));
		if (!ret)
			WRITE_ONCE(imu->sampling_frequency, programmed_rate);
	}
	mutex_unlock(&imu->xfer_lock);

	atomic_set(&imu->last_error, ret);
	if (!ret && actual)
		*actual = programmed_rate;

	return ret;
}

static int imu_hw_init(struct imu_device *imu)
{
	u8 who_am_i;
	int ret;

	ret = imu_spi_write_reg(imu, ICM20602_REG_PWR_MGMT_1,
				ICM20602_DEVICE_RESET);
	if (ret)
		return ret;
	msleep(100);

	/*
	 * ICM20602 has a dedicated I2C_IF register.  This is not the MPU-60x0
	 * USER_CTRL bit and must be programmed immediately after reset/startup.
	 */
	ret = imu_spi_write_reg(imu, ICM20602_REG_I2C_IF,
				ICM20602_I2C_IF_DIS);
	if (ret)
		return ret;

	ret = imu_spi_read_reg(imu, ICM20602_REG_WHO_AM_I, &who_am_i);
	if (ret)
		return ret;
	if (who_am_i != ICM20602_WHO_AM_I_VALUE) {
		dev_err(&imu->spi->dev,
			"WHO_AM_I mismatch: got 0x%02x, expected 0x%02x\n",
			who_am_i, ICM20602_WHO_AM_I_VALUE);
		return -ENODEV;
	}
	imu->who_am_i = who_am_i;

	ret = imu_spi_write_reg(imu, ICM20602_REG_PWR_MGMT_1,
				ICM20602_CLK_PLL_XGYRO);
	if (ret)
		return ret;
	/* Datasheet gyro drive-start time is up to 100 ms. */
	msleep(100);

	ret = imu_spi_write_reg(imu, ICM20602_REG_PWR_MGMT_2, 0x00);
	if (ret)
		return ret;
	ret = imu_spi_write_reg(imu, ICM20602_REG_ACCEL_INTEL_CTRL,
				ICM20602_ACCEL_OUTPUT_LIMIT);
	if (ret)
		return ret;
	ret = imu_spi_write_reg(imu, ICM20602_REG_INT_ENABLE, 0x00);
	if (ret)
		return ret;
	ret = imu_spi_write_reg(imu, ICM20602_REG_CONFIG,
				ICM20602_DLPF_20HZ);
	if (ret)
		return ret;
	ret = imu_spi_write_reg(imu, ICM20602_REG_GYRO_CONFIG,
				ICM20602_GYRO_FS_250DPS);
	if (ret)
		return ret;
	ret = imu_spi_write_reg(imu, ICM20602_REG_ACCEL_CONFIG,
				ICM20602_ACCEL_FS_2G);
	if (ret)
		return ret;
	ret = imu_spi_write_reg(imu, ICM20602_REG_ACCEL_CONFIG2,
				ICM20602_DLPF_20HZ);
	if (ret)
		return ret;
	ret = imu_spi_write_reg(imu, ICM20602_REG_SMPLRT_DIV,
				(ICM20602_INTERNAL_RATE_HZ /
				 ICM20602_DEFAULT_HZ) - 1U);
	if (ret)
		return ret;
	ret = imu_spi_write_reg(imu, ICM20602_REG_INT_PIN_CFG,
				ICM20602_INT_ANYRD_2CLEAR);
	if (ret)
		return ret;

	return 0;
}

static void imu_hw_stop(struct imu_device *imu)
{
	mutex_lock(&imu->xfer_lock);
	imu_spi_write_reg(imu, ICM20602_REG_INT_ENABLE, 0x00);
	imu_spi_write_reg(imu, ICM20602_REG_PWR_MGMT_1, ICM20602_SLEEP);
	mutex_unlock(&imu->xfer_lock);
}

static irqreturn_t imu_irq_handler(int irq, void *data)
{
	struct imu_device *imu = data;

	(void)irq;
	atomic64_set(&imu->irq_timestamp_ns, ktime_get_ns());

	/* SPI is sleepable; only timestamp the event and wake its thread. */
	return IRQ_WAKE_THREAD;
}

static irqreturn_t imu_irq_thread(int irq, void *data)
{
	struct imu_device *imu = data;
	struct imu_sample sample;
	u64 event_timestamp_ns;
	int ret;

	(void)irq;
	atomic64_inc(&imu->irq_count);
	if (READ_ONCE(imu->removing))
		return IRQ_HANDLED;
	event_timestamp_ns = atomic64_read(&imu->irq_timestamp_ns);

	ret = imu_read_sample(imu, &sample);
	if (ret) {
		dev_err_ratelimited(&imu->spi->dev,
				    "data-ready SPI read failed: %d\n", ret);
		return IRQ_HANDLED;
	}
	if (event_timestamp_ns)
		sample.timestamp_ns = event_timestamp_ns;

	imu_ring_push(&imu->ring, &sample);
	wake_up_interruptible(&imu->read_wq);

	return IRQ_HANDLED;
}

static int imu_debugfs_stats_show(struct seq_file *seq, void *unused)
{
	struct imu_device *imu = seq->private;
	unsigned int depth;
	unsigned int max_depth;
	u64 drop_count;
	u64 last_read_ns;

	(void)unused;
	imu_ring_get_stats(&imu->ring, &depth, &max_depth, &drop_count);
	last_read_ns = atomic64_read(&imu->last_spi_read_ns);

	seq_printf(seq, "who_am_i=0x%02x\n", imu->who_am_i);
	seq_printf(seq, "abi_version=%u\n", IMU_SAMPLE_ABI_VERSION);
	seq_printf(seq, "irq=%d\n", imu->irq);
	seq_printf(seq, "irq_count=%lld\n",
		   (long long)atomic64_read(&imu->irq_count));
	seq_printf(seq, "sample_count=%lld\n",
		   (long long)atomic64_read(&imu->sample_count));
	seq_printf(seq, "drop_count=%llu\n", (unsigned long long)drop_count);
	seq_printf(seq, "ring_depth=%u\n", depth);
	seq_printf(seq, "ring_max_depth=%u\n", max_depth);
	seq_printf(seq, "last_spi_read_us=%llu\n",
		   (unsigned long long)div_u64(last_read_ns, 1000));
	seq_printf(seq, "sampling_frequency=%u\n",
		   READ_ONCE(imu->sampling_frequency));
	seq_printf(seq, "last_error=%d\n", atomic_read(&imu->last_error));

	return 0;
}

static int imu_debugfs_stats_open(struct inode *inode, struct file *file)
{
	struct imu_device *imu = inode->i_private;
	int ret;

	mutex_lock(&imu->lifecycle_lock);
	if (imu->removing) {
		mutex_unlock(&imu->lifecycle_lock);
		return -ENODEV;
	}
	imu_device_get(imu);
	mutex_unlock(&imu->lifecycle_lock);

	ret = single_open(file, imu_debugfs_stats_show, imu);
	if (ret)
		imu_device_put(imu);

	return ret;
}

static int imu_debugfs_stats_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct imu_device *imu = seq->private;
	int ret;

	ret = single_release(inode, file);
	imu_device_put(imu);

	return ret;
}

static const struct file_operations imu_debugfs_stats_fops = {
	.owner = THIS_MODULE,
	.open = imu_debugfs_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = imu_debugfs_stats_release,
};

static void imu_debugfs_init(struct imu_device *imu)
{
	struct dentry *stats;

	imu->debugfs_dir = debugfs_create_dir(IMU_CHARDEV_NAME, NULL);
	if (IS_ERR_OR_NULL(imu->debugfs_dir)) {
		imu->debugfs_dir = NULL;
		return;
	}

	stats = debugfs_create_file("stats", 0444, imu->debugfs_dir, imu,
				    &imu_debugfs_stats_fops);
	if (IS_ERR_OR_NULL(stats)) {
		debugfs_remove_recursive(imu->debugfs_dir);
		imu->debugfs_dir = NULL;
	}
}

static void imu_debugfs_remove(struct imu_device *imu)
{
	debugfs_remove_recursive(imu->debugfs_dir);
	imu->debugfs_dir = NULL;
}

static void imu_mark_removing(struct imu_device *imu)
{
	mutex_lock(&imu->lifecycle_lock);
	imu->removing = true;
	mutex_unlock(&imu->lifecycle_lock);
	wake_up_interruptible(&imu->read_wq);
}

static void imu_irq_free(struct imu_device *imu)
{
	if (!imu->irq_requested)
		return;

	free_irq(imu->irq, imu);
	imu->irq_requested = false;
}

static int imu_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct imu_device *imu;
	int ret;

	BUILD_BUG_ON(sizeof(struct imu_sample) != IMU_SAMPLE_WIRE_SIZE);
	BUILD_BUG_ON(offsetof(struct imu_sample, reserved0) != 12);
	BUILD_BUG_ON(offsetof(struct imu_sample, timestamp_ns) != 16);
	BUILD_BUG_ON((IMU_RING_SIZE & IMU_RING_MASK) != 0);

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(struct imu_device *));
	if (!indio_dev)
		return -ENOMEM;

	imu = kzalloc(sizeof(*imu), GFP_KERNEL);
	if (!imu)
		return -ENOMEM;
	*(struct imu_device **)iio_priv(indio_dev) = imu;

	imu->spi = spi;
	imu->indio_dev = indio_dev;
	imu->irq = spi->irq;
	imu->sampling_frequency = ICM20602_DEFAULT_HZ;
	kref_init(&imu->refcount);
	mutex_init(&imu->xfer_lock);
	mutex_init(&imu->lifecycle_lock);
	init_waitqueue_head(&imu->read_wq);
	imu_ring_init(&imu->ring);
	atomic_set(&imu->last_error, 0);
	atomic64_set(&imu->irq_count, 0);
	atomic64_set(&imu->sample_count, 0);
	atomic64_set(&imu->last_spi_read_ns, 0);
	atomic64_set(&imu->irq_timestamp_ns, 0);

	spi->mode &= ~(SPI_CPOL | SPI_CPHA);
	spi->bits_per_word = 8;
	if (!spi->max_speed_hz || spi->max_speed_hz > ICM20602_MAX_SPI_HZ)
		spi->max_speed_hz = ICM20602_MAX_SPI_HZ;
	ret = spi_setup(spi);
	if (ret)
		goto err_put;

	spi_set_drvdata(spi, imu);
	ret = imu_hw_init(imu);
	if (ret) {
		dev_err(&spi->dev, "hardware initialization failed: %d\n", ret);
		goto err_hw_stop;
	}

	/* Keep INT_ENABLE clear until every userspace interface is ready. */
	if (imu->irq > 0) {
		ret = request_threaded_irq(imu->irq, imu_irq_handler,
					   imu_irq_thread, IRQF_ONESHOT,
					   dev_name(&spi->dev), imu);
		if (ret) {
			dev_err(&spi->dev, "failed to request IRQ %d: %d\n",
				imu->irq, ret);
			goto err_hw_stop;
		}
		imu->irq_requested = true;
	}

	ret = imu_iio_register(imu);
	if (ret) {
		dev_err(&spi->dev, "IIO registration failed: %d\n", ret);
		goto err_irq_stop;
	}

	if (imu->irq > 0) {
		ret = imu_char_register(imu);
		if (ret) {
			dev_err(&spi->dev, "failed to register /dev/%s: %d\n",
				IMU_CHARDEV_NAME, ret);
			goto err_registered;
		}
	}

	imu_debugfs_init(imu);

	if (imu->irq > 0) {
		ret = imu_spi_write_reg(imu, ICM20602_REG_INT_ENABLE,
					ICM20602_DATA_RDY_INT);
		if (ret) {
			dev_err(&spi->dev, "failed to enable data-ready IRQ: %d\n",
				ret);
			goto err_registered;
		}
	} else {
		dev_warn(&spi->dev,
			 "no data-ready IRQ; IIO direct reads available, /dev/%s omitted\n",
			 IMU_CHARDEV_NAME);
	}

	dev_info(&spi->dev,
		 "ICM20602 detected (WHO_AM_I=0x%02x, %u Hz, IRQ=%d)\n",
		 imu->who_am_i, imu->sampling_frequency, imu->irq);
	return 0;

err_registered:
	imu_mark_removing(imu);
	imu_hw_stop(imu);
	imu_irq_free(imu);
	imu_debugfs_remove(imu);
	imu_char_unregister(imu);
	imu_iio_unregister(imu);
	goto err_put;
err_irq_stop:
	imu_mark_removing(imu);
	imu_hw_stop(imu);
	imu_irq_free(imu);
	goto err_put;
err_hw_stop:
	imu_hw_stop(imu);
err_put:
	spi_set_drvdata(spi, NULL);
	imu_device_put(imu);
	return ret;
}

static int imu_remove(struct spi_device *spi)
{
	struct imu_device *imu = spi_get_drvdata(spi);

	imu_mark_removing(imu);
	imu_hw_stop(imu);
	imu_irq_free(imu);
	imu_debugfs_remove(imu);
	imu_char_unregister(imu);
	imu_iio_unregister(imu);
	spi_set_drvdata(spi, NULL);
	imu_device_put(imu);

	return 0;
}

static const struct of_device_id imu_of_match[] = {
	{ .compatible = "invensense,icm20602" },
	{ .compatible = "myvendor,icm20602" },
	{ }
};
MODULE_DEVICE_TABLE(of, imu_of_match);

static const struct spi_device_id imu_spi_ids[] = {
	{ "icm20602", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, imu_spi_ids);

static struct spi_driver imu_spi_driver = {
	.driver = {
		.name = IMU_DRV_NAME,
		.of_match_table = of_match_ptr(imu_of_match),
	},
	.probe = imu_probe,
	.remove = imu_remove,
	.id_table = imu_spi_ids,
};
module_spi_driver(imu_spi_driver);

MODULE_AUTHOR("i.MX6ULL IMU Attitude Project");
MODULE_DESCRIPTION("ICM20602 SPI IMU ring-buffer and IIO driver");
MODULE_LICENSE("GPL v2");
