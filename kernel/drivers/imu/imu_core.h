/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IMX6ULL_ICM20602_IMU_CORE_H
#define IMX6ULL_ICM20602_IMU_CORE_H

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/spi/spi.h>
#include <linux/wait.h>
#include <linux/iio/iio.h>

#include <imu_sample.h>

#define IMU_DRV_NAME		"icm20602_imu"
#define IMU_CHARDEV_NAME	"imu0"

/* Keep this a power of two so advancing an index is inexpensive. */
#define IMU_RING_SIZE		256U
#define IMU_RING_MASK		(IMU_RING_SIZE - 1U)

#define ICM20602_WHO_AM_I_VALUE	0x12
#define ICM20602_DEFAULT_HZ	100U

struct imu_ring {
	struct imu_sample samples[IMU_RING_SIZE];
	unsigned int head;
	unsigned int tail;
	unsigned int count;
	unsigned int max_depth;
	u64 drop_count;
	spinlock_t lock;
};

struct imu_device {
	struct spi_device *spi;
	struct iio_dev *indio_dev;
	struct miscdevice miscdev;
	struct dentry *debugfs_dir;
	struct kref refcount;

	/* Serializes all sleeping SPI transfers and register changes. */
	struct mutex xfer_lock;
	/* Serializes open() against device removal. */
	struct mutex lifecycle_lock;
	wait_queue_head_t read_wq;
	struct imu_ring ring;

	atomic_t last_error;
	atomic64_t irq_count;
	atomic64_t sample_count;
	atomic64_t last_spi_read_ns;
	atomic64_t irq_timestamp_ns;

	unsigned int sampling_frequency;
	int irq;
	u8 who_am_i;
	bool irq_requested;
	bool char_registered;
	bool removing;
};

static inline struct imu_device *imu_from_iio(struct iio_dev *indio_dev)
{
	return *(struct imu_device **)iio_priv(indio_dev);
}

void imu_device_get(struct imu_device *imu);
void imu_device_put(struct imu_device *imu);

void imu_ring_init(struct imu_ring *ring);
bool imu_ring_has_data(struct imu_ring *ring);
void imu_ring_push(struct imu_ring *ring, const struct imu_sample *sample);
unsigned int imu_ring_pop_many(struct imu_ring *ring,
			       struct imu_sample *samples,
			       unsigned int max_samples);
void imu_ring_get_stats(struct imu_ring *ring, unsigned int *depth,
			unsigned int *max_depth, u64 *drop_count);

int imu_char_register(struct imu_device *imu);
void imu_char_unregister(struct imu_device *imu);

int imu_read_sample(struct imu_device *imu, struct imu_sample *sample);
int imu_set_sampling_frequency(struct imu_device *imu, unsigned int requested,
			       unsigned int *actual);

int imu_iio_register(struct imu_device *imu);
void imu_iio_unregister(struct imu_device *imu);

#endif /* IMX6ULL_ICM20602_IMU_CORE_H */
