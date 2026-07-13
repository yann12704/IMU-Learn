// SPDX-License-Identifier: GPL-2.0
/*
 * SSD1306 128x64 I2C text display driver.
 *
 * A miscdevice is used because this device only needs one small write-only
 * character interface.  It provides the required /dev/oled0 node while
 * avoiding the extra dev_t, cdev, class and device lifetime plumbing of a
 * hand-built cdev implementation.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "font5x7.h"

#define SSD1306_DRIVER_NAME	"ssd1306_i2c"
#define SSD1306_DEVICE_NAME	"oled0"

#define SSD1306_WIDTH		128
#define SSD1306_HEIGHT		64
#define SSD1306_PAGES		(SSD1306_HEIGHT / 8)
#define SSD1306_FB_SIZE		(SSD1306_WIDTH * SSD1306_PAGES)

#define SSD1306_CTRL_COMMAND	0x00
#define SSD1306_CTRL_DATA	0x40
#define SSD1306_I2C_CHUNK	16
#define SSD1306_GLYPH_ADVANCE	(SSD1306_FONT_WIDTH + 1)
#define SSD1306_MAX_WRITE	512

struct ssd1306 {
	struct i2c_client *client;
	struct miscdevice miscdev;
	struct mutex lock;
	struct kref refcount;
	u8 framebuffer[SSD1306_FB_SIZE];
	u8 cursor_x;
	u8 cursor_page;
	bool removed;
};

static int ssd1306_write_bytes(struct ssd1306 *oled, u8 control,
			       const u8 *data, size_t len)
{
	u8 tx[SSD1306_I2C_CHUNK + 1];
	size_t offset = 0;
	int ret;

	while (offset < len) {
		size_t chunk = min_t(size_t, len - offset, SSD1306_I2C_CHUNK);

		tx[0] = control;
		memcpy(&tx[1], &data[offset], chunk);

		ret = i2c_master_send(oled->client, (const char *)tx,
				      (int)chunk + 1);
		if (ret < 0)
			return ret;
		if (ret != (int)chunk + 1)
			return -EIO;

		offset += chunk;
	}

	return 0;
}

static int ssd1306_command(struct ssd1306 *oled, u8 command)
{
	return ssd1306_write_bytes(oled, SSD1306_CTRL_COMMAND, &command, 1);
}

static int ssd1306_refresh(struct ssd1306 *oled)
{
	u8 address[3];
	unsigned int page;
	int ret;

	for (page = 0; page < SSD1306_PAGES; page++) {
		/* Page addressing mode, column 0 (low and high nibbles). */
		address[0] = (u8)(0xb0 | page);
		address[1] = 0x00;
		address[2] = 0x10;

		ret = ssd1306_write_bytes(oled, SSD1306_CTRL_COMMAND,
					  address, sizeof(address));
		if (ret)
			return ret;

		ret = ssd1306_write_bytes(oled, SSD1306_CTRL_DATA,
					  &oled->framebuffer[page * SSD1306_WIDTH],
					  SSD1306_WIDTH);
		if (ret)
			return ret;
	}

	return 0;
}

static int ssd1306_hw_init(struct ssd1306 *oled)
{
	static const u8 init_sequence[] = {
		0xae,             /* display off */
		0xd5, 0x80,       /* clock divide ratio / oscillator */
		0xa8, 0x3f,       /* multiplex ratio: 64 rows */
		0xd3, 0x00,       /* display offset */
		0x40,             /* display start line 0 */
		0x8d, 0x14,       /* enable charge pump */
		0x20, 0x02,       /* page addressing mode */
		0xa1,             /* segment remap */
		0xc8,             /* COM output scan direction */
		0xda, 0x12,       /* COM pins for 128x64 panel */
		0x81, 0x7f,       /* contrast */
		0xd9, 0xf1,       /* pre-charge period */
		0xdb, 0x40,       /* VCOMH deselect level */
		0xa4,             /* display follows framebuffer RAM */
		0xa6,             /* normal (not inverted) display */
		0xaf,             /* display on */
	};
	int ret;

	/* Give power and the panel's reset circuit time to settle. */
	msleep(20);
	ret = ssd1306_write_bytes(oled, SSD1306_CTRL_COMMAND,
				  init_sequence, sizeof(init_sequence));
	if (ret)
		return ret;

	memset(oled->framebuffer, 0, sizeof(oled->framebuffer));
	oled->cursor_x = 0;
	oled->cursor_page = 0;

	return ssd1306_refresh(oled);
}

static void ssd1306_scroll_one_line(struct ssd1306 *oled)
{
	memmove(oled->framebuffer,
		&oled->framebuffer[SSD1306_WIDTH],
		SSD1306_FB_SIZE - SSD1306_WIDTH);
	memset(&oled->framebuffer[SSD1306_FB_SIZE - SSD1306_WIDTH],
	       0, SSD1306_WIDTH);
}

static void ssd1306_newline(struct ssd1306 *oled)
{
	oled->cursor_x = 0;
	if (oled->cursor_page + 1 < SSD1306_PAGES) {
		oled->cursor_page++;
		return;
	}

	ssd1306_scroll_one_line(oled);
	oled->cursor_page = SSD1306_PAGES - 1;
}

static void ssd1306_draw_printable(struct ssd1306 *oled, u8 ch)
{
	const u8 *glyph;
	unsigned int offset;

	if (ch < SSD1306_FONT_FIRST || ch > SSD1306_FONT_LAST)
		ch = '?';

	if (oled->cursor_x + SSD1306_GLYPH_ADVANCE > SSD1306_WIDTH)
		ssd1306_newline(oled);

	offset = oled->cursor_page * SSD1306_WIDTH + oled->cursor_x;
	glyph = ssd1306_font5x7[ch - SSD1306_FONT_FIRST];
	memcpy(&oled->framebuffer[offset], glyph, SSD1306_FONT_WIDTH);
	oled->framebuffer[offset + SSD1306_FONT_WIDTH] = 0x00;
	oled->cursor_x += SSD1306_GLYPH_ADVANCE;
}

static void ssd1306_render_byte(struct ssd1306 *oled, u8 ch)
{
	unsigned int spaces;

	switch (ch) {
	case '\n':
		ssd1306_newline(oled);
		break;
	case '\r':
		oled->cursor_x = 0;
		break;
	case '\t':
		spaces = 4 - ((oled->cursor_x / SSD1306_GLYPH_ADVANCE) % 4);
		while (spaces--)
			ssd1306_draw_printable(oled, ' ');
		break;
	case '\b':
		if (oled->cursor_x >= SSD1306_GLYPH_ADVANCE) {
			oled->cursor_x -= SSD1306_GLYPH_ADVANCE;
			memset(&oled->framebuffer[oled->cursor_page * SSD1306_WIDTH +
					  oled->cursor_x], 0,
			       SSD1306_GLYPH_ADVANCE);
		}
		break;
	default:
		ssd1306_draw_printable(oled, ch);
		break;
	}
}

static void ssd1306_release_ref(struct kref *ref)
{
	struct ssd1306 *oled = container_of(ref, struct ssd1306, refcount);

	mutex_destroy(&oled->lock);
	kfree(oled);
}

static int ssd1306_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct ssd1306 *oled = container_of(misc, struct ssd1306, miscdev);

	(void)inode;
	kref_get(&oled->refcount);
	file->private_data = oled;

	return 0;
}

static int ssd1306_release(struct inode *inode, struct file *file)
{
	struct ssd1306 *oled = file->private_data;

	(void)inode;
	kref_put(&oled->refcount, ssd1306_release_ref);

	return 0;
}

static ssize_t ssd1306_write(struct file *file, const char __user *user_buf,
			     size_t count, loff_t *ppos)
{
	struct ssd1306 *oled = file->private_data;
	u8 *text;
	size_t i;
	int ret;

	(void)ppos;
	if (!count)
		return 0;
	if (count > SSD1306_MAX_WRITE)
		return -EMSGSIZE;

	text = kmalloc(count, GFP_KERNEL);
	if (!text)
		return -ENOMEM;
	if (copy_from_user(text, user_buf, count)) {
		kfree(text);
		return -EFAULT;
	}

	ret = mutex_lock_interruptible(&oled->lock);
	if (ret) {
		kfree(text);
		return ret;
	}

	if (oled->removed) {
		ret = -ENODEV;
		goto out_unlock;
	}

	/* A write is one complete frame, which keeps periodic updates atomic. */
	memset(oled->framebuffer, 0, sizeof(oled->framebuffer));
	oled->cursor_x = 0;
	oled->cursor_page = 0;
	for (i = 0; i < count; i++)
		ssd1306_render_byte(oled, text[i]);

	ret = ssd1306_refresh(oled);
	if (!ret)
		ret = (int)count;

out_unlock:
	mutex_unlock(&oled->lock);
	kfree(text);
	return ret;
}

static const struct file_operations ssd1306_fops = {
	.owner = THIS_MODULE,
	.open = ssd1306_open,
	.write = ssd1306_write,
	.release = ssd1306_release,
	.llseek = no_llseek,
};

static int ssd1306_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct ssd1306 *oled;
	int ret;

	(void)id;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	oled = kzalloc(sizeof(*oled), GFP_KERNEL);
	if (!oled)
		return -ENOMEM;

	oled->client = client;
	mutex_init(&oled->lock);
	kref_init(&oled->refcount);
	i2c_set_clientdata(client, oled);

	ret = ssd1306_hw_init(oled);
	if (ret) {
		dev_err(&client->dev, "SSD1306 initialization failed: %d\n", ret);
		goto err_power_off;
	}

	oled->miscdev.minor = MISC_DYNAMIC_MINOR;
	oled->miscdev.name = SSD1306_DEVICE_NAME;
	oled->miscdev.fops = &ssd1306_fops;
	oled->miscdev.parent = &client->dev;

	ret = misc_register(&oled->miscdev);
	if (ret) {
		dev_err(&client->dev, "failed to register /dev/%s: %d\n",
			SSD1306_DEVICE_NAME, ret);
		goto err_power_off;
	}

	dev_info(&client->dev, "SSD1306 128x64 registered as /dev/%s\n",
		 SSD1306_DEVICE_NAME);
	return 0;

err_power_off:
	/* Best effort: this can also fail when initialization failed on I2C. */
	ssd1306_command(oled, 0xae);
	i2c_set_clientdata(client, NULL);
	kref_put(&oled->refcount, ssd1306_release_ref);
	return ret;
}

static int ssd1306_remove(struct i2c_client *client)
{
	struct ssd1306 *oled = i2c_get_clientdata(client);

	/* misc_deregister serializes against new opens in the misc core. */
	misc_deregister(&oled->miscdev);

	mutex_lock(&oled->lock);
	oled->removed = true;
	ssd1306_command(oled, 0xae);
	mutex_unlock(&oled->lock);

	i2c_set_clientdata(client, NULL);
	kref_put(&oled->refcount, ssd1306_release_ref);
	return 0;
}

static const struct of_device_id ssd1306_of_match[] = {
	{ .compatible = "myvendor,ssd1306" },
	{ }
};
MODULE_DEVICE_TABLE(of, ssd1306_of_match);

static const struct i2c_device_id ssd1306_i2c_ids[] = {
	{ "ssd1306", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ssd1306_i2c_ids);

static struct i2c_driver ssd1306_driver = {
	.driver = {
		.name = SSD1306_DRIVER_NAME,
		.of_match_table = of_match_ptr(ssd1306_of_match),
	},
	.probe = ssd1306_probe,
	.remove = ssd1306_remove,
	.id_table = ssd1306_i2c_ids,
};
module_i2c_driver(ssd1306_driver);

MODULE_AUTHOR("i.MX6ULL IMU attitude project");
MODULE_DESCRIPTION("SSD1306 128x64 I2C text display driver");
MODULE_LICENSE("GPL v2");
