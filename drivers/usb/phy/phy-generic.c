/*
 * NOP USB transceiver for all USB transceiver which are either built-in
 * into USB IP or which are mostly autonomous.
 *
 * Copyright (C) 2009 Texas Instruments Inc
 * Author: Ajay Kumar Gupta <ajay.gupta@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Current status:
 *	This provides a "nop" transceiver for PHYs which are
 *	autonomous such as isp1504, isp1707, etc.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/usb/gadget.h>
#include <linux/usb/otg.h>
#include <linux/usb/usb_phy_generic.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#include "phy-generic.h"

#define VBUS_IRQ_FLAGS \
	(IRQF_SHARED | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | \
		IRQF_ONESHOT)

struct platform_device *usb_phy_generic_register(void)
{
	return platform_device_register_simple("usb_phy_generic",
			PLATFORM_DEVID_AUTO, NULL, 0);
}
EXPORT_SYMBOL_GPL(usb_phy_generic_register);

void usb_phy_generic_unregister(struct platform_device *pdev)
{
	platform_device_unregister(pdev);
}
EXPORT_SYMBOL_GPL(usb_phy_generic_unregister);

static int nop_set_suspend(struct usb_phy *x, int suspend)
{
	return 0;
}

static void nop_reset(struct usb_phy_generic *nop)
{
	if (!nop->gpiod_reset)
		return;

	gpiod_set_value(nop->gpiod_reset, 1);
	usleep_range(10000, 20000);
	gpiod_set_value(nop->gpiod_reset, 0);
}

/* interface to regulator framework */
static void nop_set_vbus_draw(struct usb_phy_generic *nop, unsigned mA)
{
	struct regulator *vbus_draw = nop->vbus_draw;
	int enabled;
	int ret;

	if (!vbus_draw)
		return;

	enabled = nop->vbus_draw_enabled;
	if (mA) {
		regulator_set_current_limit(vbus_draw, 0, 1000 * mA);
		if (!enabled) {
			ret = regulator_enable(vbus_draw);
			if (ret < 0)
				return;
			nop->vbus_draw_enabled = 1;
		}
	} else {
		if (enabled) {
			ret = regulator_disable(vbus_draw);
			if (ret < 0)
				return;
			nop->vbus_draw_enabled = 0;
		}
	}
	nop->mA = mA;
}


static irqreturn_t nop_gpio_vbus_thread(int irq, void *data)
{
	struct usb_phy_generic *nop = data;
	struct usb_otg *otg = nop->phy.otg;
	int vbus, status;

	vbus = gpiod_get_value(nop->gpiod_vbus);
	if ((vbus ^ nop->vbus) == 0)
		return IRQ_HANDLED;
	nop->vbus = vbus;

	if (vbus) {
		status = USB_EVENT_VBUS;
		otg->state = OTG_STATE_B_PERIPHERAL;
		nop->phy.last_event = status;
		usb_gadget_vbus_connect(otg->gadget);

		/* drawing a "unit load" is *always* OK, except for OTG */
		nop_set_vbus_draw(nop, 100);

		atomic_notifier_call_chain(&nop->phy.notifier, status,
					   otg->gadget);
	} else {
		nop_set_vbus_draw(nop, 0);

		usb_gadget_vbus_disconnect(otg->gadget);
		status = USB_EVENT_NONE;
		otg->state = OTG_STATE_B_IDLE;
		nop->phy.last_event = status;

		atomic_notifier_call_chain(&nop->phy.notifier, status,
					   otg->gadget);
	}
	return IRQ_HANDLED;
}

int usb_gen_phy_init(struct usb_phy *phy)
{
	struct usb_phy_generic *nop = dev_get_drvdata(phy->dev);

	if (!IS_ERR(nop->vcc)) {
		if (regulator_enable(nop->vcc))
			dev_err(phy->dev, "Failed to enable power\n");
	}

	if (!IS_ERR(nop->clk))
		clk_prepare_enable(nop->clk);

	nop_reset(nop);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_gen_phy_init);

void usb_gen_phy_shutdown(struct usb_phy *phy)
{
	struct usb_phy_generic *nop = dev_get_drvdata(phy->dev);

	gpiod_set_value(nop->gpiod_reset, 1);

	if (!IS_ERR(nop->clk))
		clk_disable_unprepare(nop->clk);

	if (!IS_ERR(nop->vcc)) {
		if (regulator_disable(nop->vcc))
			dev_err(phy->dev, "Failed to disable power\n");
	}
}
EXPORT_SYMBOL_GPL(usb_gen_phy_shutdown);

static int nop_set_peripheral(struct usb_otg *otg, struct usb_gadget *gadget)
{
	if (!otg)
		return -ENODEV;

	if (!gadget) {
		otg->gadget = NULL;
		return -ENODEV;
	}

	otg->gadget = gadget;
	otg->state = OTG_STATE_B_IDLE;
	return 0;
}

static int nop_set_host(struct usb_otg *otg, struct usb_bus *host)
{
	if (!otg)
		return -ENODEV;

	if (!host) {
		otg->host = NULL;
		return -ENODEV;
	}

	otg->host = host;
	return 0;
}

#define MYDBG(fmt, args...) pr_warn("USB_FIXME, <%s(), %d> " fmt, __func__, __LINE__, ## args)
int usb_phy_gen_create_phy(struct device *dev, struct usb_phy_generic *nop,
		struct usb_phy_generic_platform_data *pdata)
{
	enum usb_phy_type type = USB_PHY_TYPE_USB2;
	int err = 0;

	u32 clk_rate = 0;
	bool needs_vcc = false;

	if (dev->of_node) {
		struct device_node *node = dev->of_node;

		if (of_property_read_u32(node, "clock-frequency", &clk_rate))
			clk_rate = 0;

		needs_vcc = of_property_read_bool(node, "vcc-supply");
		nop->gpiod_reset = devm_gpiod_get_optional(dev, "reset",
							   GPIOD_ASIS);
		err = PTR_ERR_OR_ZERO(nop->gpiod_reset);
		if (!err) {
			MYDBG("ORG\n");
			nop->gpiod_vbus = devm_gpiod_get_optional(dev,
							 "vbus-detect",
							 GPIOD_ASIS);
			err = PTR_ERR_OR_ZERO(nop->gpiod_vbus);
		}
		MYDBG("ORG, err<%d>\n", err);
	} else if (pdata) {
		type = pdata->type;
		clk_rate = pdata->clk_rate;
		needs_vcc = pdata->needs_vcc;
		if (gpio_is_valid(pdata->gpio_reset)) {
			err = devm_gpio_request_one(dev, pdata->gpio_reset,
						    GPIOF_ACTIVE_LOW,
						    dev_name(dev));
			if (!err)
				nop->gpiod_reset =
					gpio_to_desc(pdata->gpio_reset);
		}
		nop->gpiod_vbus = pdata->gpiod_vbus;
	}

	if (err == -EPROBE_DEFER) {
		return -EPROBE_DEFER;
	}
	if (err) {
		dev_err(dev, "Error requesting RESET or VBUS GPIO\n");
		return err;
	}
	if (nop->gpiod_reset)
		gpiod_direction_output(nop->gpiod_reset, 1);

	nop->phy.otg = devm_kzalloc(dev, sizeof(*nop->phy.otg),
			GFP_KERNEL);
	if (!nop->phy.otg)
		return -ENOMEM;

	nop->clk = devm_clk_get(dev, "main_clk");
	if (IS_ERR(nop->clk)) {
		dev_dbg(dev, "Can't get phy clock: %ld\n",
					PTR_ERR(nop->clk));
	}

	if (!IS_ERR(nop->clk) && clk_rate) {
		err = clk_set_rate(nop->clk, clk_rate);
		if (err) {
			dev_err(dev, "Error setting clock rate\n");
			return err;
		}
	}

	nop->vcc = devm_regulator_get(dev, "vcc");
	if (IS_ERR(nop->vcc)) {
		dev_dbg(dev, "Error getting vcc regulator: %ld\n",
					PTR_ERR(nop->vcc));
		if (needs_vcc)
			return -EPROBE_DEFER;
	}

	nop->dev		= dev;
	nop->phy.dev		= nop->dev;
	nop->phy.label		= "nop-xceiv";
	nop->phy.set_suspend	= nop_set_suspend;
	nop->phy.type		= type;

	nop->phy.otg->state		= OTG_STATE_UNDEFINED;
	nop->phy.otg->usb_phy		= &nop->phy;
	nop->phy.otg->set_host		= nop_set_host;
	nop->phy.otg->set_peripheral	= nop_set_peripheral;

	return 0;
}
EXPORT_SYMBOL_GPL(usb_phy_gen_create_phy);

static int usb_phy_generic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct usb_phy_generic	*nop;
	int err;

	nop = devm_kzalloc(dev, sizeof(*nop), GFP_KERNEL);
	if (!nop)
		return -ENOMEM;

	err = usb_phy_gen_create_phy(dev, nop, dev_get_platdata(&pdev->dev));
	if (err)
		return err;
	if (nop->gpiod_vbus) {
		err = devm_request_threaded_irq(&pdev->dev,
						gpiod_to_irq(nop->gpiod_vbus),
						NULL, nop_gpio_vbus_thread,
						VBUS_IRQ_FLAGS, "vbus_detect",
						nop);
		if (err) {
			dev_err(&pdev->dev, "can't request irq %i, err: %d\n",
				gpiod_to_irq(nop->gpiod_vbus), err);
			return err;
		}
	}

	nop->phy.init		= usb_gen_phy_init;
	nop->phy.shutdown	= usb_gen_phy_shutdown;

	err = usb_add_phy_dev(&nop->phy);
	if (err) {
		dev_err(&pdev->dev, "can't register transceiver, err: %d\n",
			err);
		return err;
	}

	platform_set_drvdata(pdev, nop);

	return 0;
}

static int usb_phy_generic_remove(struct platform_device *pdev)
{
	struct usb_phy_generic *nop = platform_get_drvdata(pdev);

	usb_remove_phy(&nop->phy);

	return 0;
}

static const struct of_device_id nop_xceiv_dt_ids[] = {
	{ .compatible = "usb-nop-xceiv" },
	{ }
};

MODULE_DEVICE_TABLE(of, nop_xceiv_dt_ids);

static struct platform_driver usb_phy_generic_driver = {
	.probe		= usb_phy_generic_probe,
	.remove		= usb_phy_generic_remove,
	.driver		= {
		.name	= "usb_phy_generic",
		.of_match_table = nop_xceiv_dt_ids,
	},
};

static int __init usb_phy_generic_init(void)
{
	return platform_driver_register(&usb_phy_generic_driver);
}
subsys_initcall(usb_phy_generic_init);

static void __exit usb_phy_generic_exit(void)
{
	platform_driver_unregister(&usb_phy_generic_driver);
}
module_exit(usb_phy_generic_exit);

MODULE_ALIAS("platform:usb_phy_generic");
MODULE_AUTHOR("Texas Instruments Inc");
MODULE_DESCRIPTION("NOP USB Transceiver driver");
MODULE_LICENSE("GPL");
