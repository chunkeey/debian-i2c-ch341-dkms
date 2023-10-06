// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO cell driver for the CH341A and CH341B chips.
 *
 * Copyright 2022, Frank Zago
 * Copyright (c) 2017 Gunar Schorcht (gunar@schorcht.net)
 * Copyright (c) 2016 Tse Lun Bien
 * Copyright (c) 2014 Marco Gittler
 * Copyright (c) 2006-2007 Till Harbaum (Till@Harbaum.org)
 */

/*
 * Notes.
 *
 * For the CH341, 0=IN, 1=OUT, but for the GPIO subsystem, 1=IN and
 * 0=OUT. Translation happens in a couple places.
 */

#include <linux/gpio/driver.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/usb.h>
#include "ch341.h"

#define CH341_GPIO_NUM_PINS         16    /* Number of GPIO pins */

/* GPIO chip commands */
#define CH341_PARA_CMD_STS          0xA0  /* Get pins status */
#define CH341_CMD_UIO_STREAM        0xAB  /* pin IO stream command */

#define CH341_CMD_UIO_STM_OUT       0x80  /* pin IO interface OUT command (D0~D5) */
#define CH341_CMD_UIO_STM_DIR       0x40  /* pin IO interface DIR command (D0~D5) */
#define CH341_CMD_UIO_STM_END       0x20  /* pin IO interface END command */

#define CH341_USB_MAX_INTR_SIZE 8

struct ch341_gpio {
	struct gpio_chip gpio;
	struct mutex gpio_lock;
	u16 gpio_dir;		/* 1 bit per pin, 0=IN, 1=OUT. */
	u16 gpio_last_read;	/* last GPIO values read */
	u16 gpio_last_written;	/* last GPIO values written */
	union {
		u8 gpio_buf[SEG_SIZE];
		__le16 gpio_buf_status;
	};

	struct urb *irq_urb;
	struct usb_anchor irq_urb_out;
	u8 irq_buf[CH341_USB_MAX_INTR_SIZE];
	struct irq_chip irq_chip;

	struct ch341_ddata *ddata;
};

/*
 * Masks to describe the 16 GPIOs. Pins D0 to D5 (mapped to GPIOs 0 to
 * 5) can do input/output, but the other pins are input-only.
 */
static const u16 pin_can_output = 0b111111;

/* Only GPIO 10 (INT# line) has hardware interrupt */
#define CH341_GPIO_INT_LINE 10

/* Send a command and get a reply if requested */
static int gpio_transfer(struct ch341_gpio *dev, int out_len, int in_len)
{
	struct ch341_ddata *ddata = dev->ddata;
	int actual;
	int ret;

	mutex_lock(&ddata->usb_lock);

	ret = usb_bulk_msg(ddata->usb_dev,
			   usb_sndbulkpipe(ddata->usb_dev, ddata->ep_out),
			   dev->gpio_buf, out_len,
			   &actual, DEFAULT_TIMEOUT_MS);
	if (ret < 0)
		goto out_unlock;

	if (in_len == 0)
		goto out_unlock;

	ret = usb_bulk_msg(ddata->usb_dev,
			   usb_rcvbulkpipe(ddata->usb_dev, ddata->ep_in),
			   dev->gpio_buf, SEG_SIZE, &actual, DEFAULT_TIMEOUT_MS);

out_unlock:
	mutex_unlock(&ddata->usb_lock);

	if (ret < 0)
		return ret;

	return actual;
}

/* Read the GPIO line status. */
static int read_inputs(struct ch341_gpio *dev)
{
	int ret;

	mutex_lock(&dev->gpio_lock);

	dev->gpio_buf[0] = CH341_PARA_CMD_STS;

	ret = gpio_transfer(dev, 1, 1);

	/*
	 * The status command returns 6 bytes of data. Byte 0 has
	 * status for lines 0 to 7, and byte 1 is lines 8 to 15. The
	 * 3rd has the status for the SCL/SDA/SCK pins. The 4th byte
	 * might have some remaining pin status. Byte 5 and 6 content
	 * is unknown.
	 */
	if (ret == 6)
		dev->gpio_last_read = le16_to_cpu(dev->gpio_buf_status);
	else
		ret = -EIO;

	mutex_unlock(&dev->gpio_lock);

	if (ret < 0)
		return ret;

	return 0;
}

static int ch341_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct ch341_gpio *dev = gpiochip_get_data(chip);
	int ret;

	ret = read_inputs(dev);
	if (ret)
		return ret;

	return !!(dev->gpio_last_read & BIT(offset));
}

static int ch341_gpio_get_multiple(struct gpio_chip *chip,
				   unsigned long *mask, unsigned long *bits)
{
	struct ch341_gpio *dev = gpiochip_get_data(chip);
	int ret;

	ret = read_inputs(dev);
	if (ret)
		return ret;

	*bits = dev->gpio_last_read & *mask;

	return 0;
}

static void write_outputs(struct ch341_gpio *dev)
{
	mutex_lock(&dev->gpio_lock);

	/* Only the first 6 lines can output. */
	dev->gpio_buf[0] = CH341_CMD_UIO_STREAM;
	dev->gpio_buf[1] = CH341_CMD_UIO_STM_DIR | (dev->gpio_dir & pin_can_output);
	dev->gpio_buf[2] = CH341_CMD_UIO_STM_OUT |
		(dev->gpio_last_written & dev->gpio_dir & pin_can_output);
	dev->gpio_buf[3] = CH341_CMD_UIO_STM_END;

	gpio_transfer(dev, 4, 0);

	mutex_unlock(&dev->gpio_lock);
}

static void ch341_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct ch341_gpio *dev = gpiochip_get_data(chip);

	if (value)
		dev->gpio_last_written |= BIT(offset);
	else
		dev->gpio_last_written &= ~BIT(offset);

	write_outputs(dev);
}

static void ch341_gpio_set_multiple(struct gpio_chip *chip,
				    unsigned long *mask, unsigned long *bits)
{
	struct ch341_gpio *dev = gpiochip_get_data(chip);

	dev->gpio_last_written = (dev->gpio_last_written & ~*mask) | (*bits & *mask);

	write_outputs(dev);
}

static int ch341_gpio_get_direction(struct gpio_chip *chip,
				    unsigned int offset)
{
	struct ch341_gpio *dev = gpiochip_get_data(chip);

	return !(dev->gpio_dir & BIT(offset));
}

static int ch341_gpio_direction_input(struct gpio_chip *chip,
				      unsigned int offset)
{
	struct ch341_gpio *dev = gpiochip_get_data(chip);

	dev->gpio_dir &= ~BIT(offset);

	write_outputs(dev);

	return 0;
}

static int ch341_gpio_direction_output(struct gpio_chip *chip,
				       unsigned int offset, int value)
{
	struct ch341_gpio *dev = gpiochip_get_data(chip);
	u16 mask = BIT(offset);

	if (!(pin_can_output & mask))
		return -EINVAL;

	dev->gpio_dir |= mask;

	ch341_gpio_set(chip, offset, value);

	return 0;
}

static void ch341_complete_intr_urb(struct urb *urb)
{
	struct ch341_gpio *dev = urb->context;
	int ret;

	if (urb->status) {
		usb_unanchor_urb(dev->irq_urb);
	} else {
		/*
		 * Data is 8 bytes. Byte 0 might be the length of
		 * significant data, which is 3 more bytes. Bytes 1
		 * and 2, and possibly 3, are the pin status. The byte
		 * order is different than for the GET_STATUS
		 * command. Byte 1 is GPIOs 8 to 15, and byte 2 is
		 * GPIOs 0 to 7.
		 */

		handle_nested_irq(irq_find_mapping(dev->gpio.irq.domain,
						   CH341_GPIO_INT_LINE));

		ret = usb_submit_urb(dev->irq_urb, GFP_ATOMIC);
		if (ret)
			usb_unanchor_urb(dev->irq_urb);
	}
}

static int ch341_gpio_irq_set_type(struct irq_data *data, unsigned int flow_type)
{
	const unsigned long offset = irqd_to_hwirq(data);

	if (offset != CH341_GPIO_INT_LINE || flow_type != IRQ_TYPE_EDGE_RISING)
		return -EINVAL;

	return 0;
}

static void ch341_gpio_irq_enable(struct irq_data *data)
{
	struct ch341_gpio *dev = irq_data_get_irq_chip_data(data);
	int ret;

	/*
	 * The URB might have just been unlinked in
	 * ch341_gpio_irq_disable, but the completion handler hasn't
	 * been called yet.
	 */
	if (!usb_wait_anchor_empty_timeout(&dev->irq_urb_out, 5000))
		usb_kill_anchored_urbs(&dev->irq_urb_out);

	usb_anchor_urb(dev->irq_urb, &dev->irq_urb_out);
	ret = usb_submit_urb(dev->irq_urb, GFP_KERNEL);
	if (ret)
		usb_unanchor_urb(dev->irq_urb);
}

static void ch341_gpio_irq_disable(struct irq_data *data)
{
	struct ch341_gpio *dev = irq_data_get_irq_chip_data(data);

	usb_unlink_urb(dev->irq_urb);
}

static int ch341_gpio_remove(struct platform_device *pdev)
{
	struct ch341_gpio *dev = platform_get_drvdata(pdev);

	usb_kill_anchored_urbs(&dev->irq_urb_out);
	gpiochip_remove(&dev->gpio);
	usb_free_urb(dev->irq_urb);

	return 0;
}

static const struct irq_chip ch341_irqchip = {
	.name = "CH341",
	.irq_set_type = ch341_gpio_irq_set_type,
	.irq_enable = ch341_gpio_irq_enable,
	.irq_disable = ch341_gpio_irq_disable,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int ch341_gpio_probe(struct platform_device *pdev)
{
	struct ch341_ddata *ddata = dev_get_drvdata(pdev->dev.parent);
	struct gpio_irq_chip *girq;
	struct ch341_gpio *dev;
	struct gpio_chip *gpio;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (dev == NULL)
		return -ENOMEM;

	platform_set_drvdata(pdev, dev);
	dev->ddata = ddata;
	mutex_init(&dev->gpio_lock);

	gpio = &dev->gpio;
	gpio->label = dev_name(&pdev->dev);
	gpio->parent = &pdev->dev;
	gpio->owner = THIS_MODULE;
	gpio->get_direction = ch341_gpio_get_direction;
	gpio->direction_input = ch341_gpio_direction_input;
	gpio->direction_output = ch341_gpio_direction_output;
	gpio->get = ch341_gpio_get;
	gpio->get_multiple = ch341_gpio_get_multiple;
	gpio->set = ch341_gpio_set;
	gpio->set_multiple = ch341_gpio_set_multiple;
	gpio->base = -1;
	gpio->ngpio = CH341_GPIO_NUM_PINS;
	gpio->can_sleep = true;

	girq = &gpio->irq;
	gpio_irq_chip_set_chip(girq, &ch341_irqchip);
	girq->handler = handle_simple_irq;
	girq->default_type = IRQ_TYPE_NONE;

	/* Create an URB for handling interrupt */
	dev->irq_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->irq_urb)
		return dev_err_probe(&pdev->dev, -ENOMEM, "Cannot allocate the int URB\n");

	usb_fill_int_urb(dev->irq_urb, ddata->usb_dev,
			 usb_rcvintpipe(ddata->usb_dev, ddata->ep_intr),
			 dev->irq_buf, CH341_USB_MAX_INTR_SIZE,
			 ch341_complete_intr_urb, dev, ddata->ep_intr_interval);

	init_usb_anchor(&dev->irq_urb_out);

	ret = gpiochip_add_data(gpio, dev);
	if (ret) {
		ret = dev_err_probe(&pdev->dev, ret, "Could not add GPIO\n");
		goto release_urb;
	}

	return 0;

release_urb:
	usb_free_urb(dev->irq_urb);

	return ret;
}

static struct platform_driver ch341_gpio_driver = {
	.driver.name	= "ch341-gpio",
	.probe		= ch341_gpio_probe,
	.remove		= ch341_gpio_remove,
};
module_platform_driver(ch341_gpio_driver);

MODULE_AUTHOR("Frank Zago <frank@zago.net>");
MODULE_DESCRIPTION("CH341 USB to GPIO");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ch341-gpio");
