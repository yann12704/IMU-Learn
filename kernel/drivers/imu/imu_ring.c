// SPDX-License-Identifier: GPL-2.0
/* Ring buffer and /dev/imu0 streaming interface for the ICM20602 driver. */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "imu_core.h"

void imu_ring_init(struct imu_ring *ring)
{
	memset(ring, 0, sizeof(*ring));
	spin_lock_init(&ring->lock);
}

bool imu_ring_has_data(struct imu_ring *ring)
{
	unsigned long flags;
	bool has_data;

	spin_lock_irqsave(&ring->lock, flags);
	has_data = ring->count != 0;
	spin_unlock_irqrestore(&ring->lock, flags);

	return has_data;
}

void imu_ring_push(struct imu_ring *ring, const struct imu_sample *sample)
{
	unsigned long flags;

	spin_lock_irqsave(&ring->lock, flags);
	if (ring->count == IMU_RING_SIZE) {
		/* Preserve the newest motion data by replacing the oldest sample. */
		ring->tail = (ring->tail + 1U) & IMU_RING_MASK;
		ring->drop_count++;
	} else {
		ring->count++;
	}

	ring->samples[ring->head] = *sample;
	ring->head = (ring->head + 1U) & IMU_RING_MASK;
	if (ring->count > ring->max_depth)
		ring->max_depth = ring->count;
	spin_unlock_irqrestore(&ring->lock, flags);
}

unsigned int imu_ring_pop_many(struct imu_ring *ring,
			       struct imu_sample *samples,
			       unsigned int max_samples)
{
	unsigned long flags;
	unsigned int copied = 0;

	spin_lock_irqsave(&ring->lock, flags);
	while (copied < max_samples && ring->count) {
		samples[copied++] = ring->samples[ring->tail];
		ring->tail = (ring->tail + 1U) & IMU_RING_MASK;
		ring->count--;
	}
	spin_unlock_irqrestore(&ring->lock, flags);

	return copied;
}

void imu_ring_get_stats(struct imu_ring *ring, unsigned int *depth,
			unsigned int *max_depth, u64 *drop_count)
{
	unsigned long flags;

	spin_lock_irqsave(&ring->lock, flags);
	*depth = ring->count;
	*max_depth = ring->max_depth;
	*drop_count = ring->drop_count;
	spin_unlock_irqrestore(&ring->lock, flags);
}

static int imu_chr_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct imu_device *imu = container_of(miscdev, struct imu_device,
					      miscdev);
	int ret = 0;

	mutex_lock(&imu->lifecycle_lock);
	if (imu->removing) {
		ret = -ENODEV;
	} else {
		imu_device_get(imu);
		file->private_data = imu;
	}
	mutex_unlock(&imu->lifecycle_lock);
	if (ret)
		return ret;

	return nonseekable_open(inode, file);
}

static int imu_chr_release(struct inode *inode, struct file *file)
{
	struct imu_device *imu = file->private_data;

	(void)inode;
	imu_device_put(imu);

	return 0;
}

static ssize_t imu_chr_read(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct imu_device *imu = file->private_data;
	struct imu_sample sample;
	unsigned int max_samples;
	unsigned int copied_samples = 0;
	ssize_t ret;

	(void)ppos;
	if (count < sizeof(sample))
		return -EINVAL;

	max_samples = min_t(size_t, count / sizeof(sample), IMU_RING_SIZE);

	for (;;) {
		if (READ_ONCE(imu->removing)) {
			if (copied_samples)
				return copied_samples * sizeof(sample);
			return -ENODEV;
		}

		if (imu_ring_pop_many(&imu->ring, &sample, 1U)) {
			if (copy_to_user(buf + copied_samples * sizeof(sample),
					 &sample, sizeof(sample))) {
				if (copied_samples)
					return copied_samples * sizeof(sample);
				return -EFAULT;
			}

			++copied_samples;
			if (copied_samples == max_samples)
				return copied_samples * sizeof(sample);
			continue;
		}

		if (copied_samples)
			return copied_samples * sizeof(sample);

		if (file->f_flags & O_NONBLOCK) {
			return -EAGAIN;
		}

		ret = wait_event_interruptible(imu->read_wq,
				READ_ONCE(imu->removing) ||
				imu_ring_has_data(&imu->ring));
		if (ret)
			return ret;
	}
}

static unsigned int imu_chr_poll(struct file *file, poll_table *wait)
{
	struct imu_device *imu = file->private_data;
	unsigned int mask = 0;

	poll_wait(file, &imu->read_wq, wait);
	if (imu_ring_has_data(&imu->ring))
		mask |= POLLIN | POLLRDNORM;
	if (READ_ONCE(imu->removing))
		mask |= POLLERR | POLLHUP;

	return mask;
}

static const struct file_operations imu_fops = {
	.owner = THIS_MODULE,
	.open = imu_chr_open,
	.release = imu_chr_release,
	.read = imu_chr_read,
	.poll = imu_chr_poll,
	.llseek = no_llseek,
};

int imu_char_register(struct imu_device *imu)
{
	int ret;

	imu->miscdev.minor = MISC_DYNAMIC_MINOR;
	imu->miscdev.name = IMU_CHARDEV_NAME;
	imu->miscdev.fops = &imu_fops;
	imu->miscdev.parent = &imu->spi->dev;

	ret = misc_register(&imu->miscdev);
	if (!ret)
		imu->char_registered = true;

	return ret;
}

void imu_char_unregister(struct imu_device *imu)
{
	if (!imu->char_registered)
		return;

	misc_deregister(&imu->miscdev);
	imu->char_registered = false;
}
