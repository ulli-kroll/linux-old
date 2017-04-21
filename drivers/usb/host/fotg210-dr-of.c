/*
 * Minimal setup code needed to support Faraday FOTG dual role
 * usb controller
 *
 * This is a generic driver only, to use this driver  special SoC glue
 * code must be added i.e. to use VBUS control
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/fotg210_of.h>
#include <linux/bitops.h>
/* For Cortina Gemini */
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>


struct fotg210_dev_data {
	char *dr_mode;		/* controller mode */
	char *drivers[3];	/* drivers to instantiate for this mode */
	enum fotg210_operating_modes op_mode;	/* operating mode */
};

static struct fotg210_dev_data dr_mode_data[] = {
	{
		.dr_mode = "host",
		.drivers = { "fotg210-hcd", NULL, NULL, },
		.op_mode = FOTG210_DR_HOST,
	},
	{
		.dr_mode = "peripheral",
		.drivers = { "fotg210-udc", NULL, NULL, },
		.op_mode = FOTG210_DR_DEVICE,
	},
};

static struct fotg210_dev_data *get_dr_mode_data(struct device_node *np)
{
	const unsigned char *prop;
	int i;

	prop = of_get_property(np, "dr_mode", NULL);
	if (prop) {
		for (i = 0; i < ARRAY_SIZE(dr_mode_data); i++) {
			if (!strcmp(prop, dr_mode_data[i].dr_mode))
				return &dr_mode_data[i];
		}
	}
	pr_warn("%s: Invalid 'dr_mode' property, fallback to host mode\n",
		np->full_name);
	return &dr_mode_data[0]; /* mode not specified, use host */
}

static struct platform_device *fotg2_device_register(
					struct platform_device *ofdev,
					struct fotg210_platform_data *pdata,
					const char *name, int id)
{
	struct platform_device *pdev;
	const struct resource *res = ofdev->resource;
	unsigned int num = ofdev->num_resources;
	int retval;

	pdev = platform_device_alloc(name, id);
	if (!pdev) {
		retval = -ENOMEM;
		goto error;
	}

	pdev->dev.parent = &ofdev->dev;

	pdev->dev.coherent_dma_mask = ofdev->dev.coherent_dma_mask;

	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &ofdev->dev.coherent_dma_mask;
	else
		dma_set_mask(&pdev->dev, DMA_BIT_MASK(32));

	retval = platform_device_add_data(pdev, pdata, sizeof(*pdata));
	if (retval)
		goto error;

	if (num) {
		retval = platform_device_add_resources(pdev, res, num);
		if (retval)
			goto error;
	}

	retval = platform_device_add(pdev);
	if (retval)
		goto error;

	return pdev;

error:
	platform_device_put(pdev);
	return ERR_PTR(retval);
}

static const struct of_device_id fotg210_dr_of_match[];

/*
 * Gemini-specific initialization function, only executed on the
 * Gemini SoC using the global misc control register.
 */
#define GEMINI_GLOBAL_MISC_CTRL		0x30
#define GEMINI_MISC_USB0_WAKEUP		BIT(14)
#define GEMINI_MISC_USB1_WAKEUP		BIT(15)
#define GEMINI_MISC_USB0_VBUS_ON	BIT(22)
#define GEMINI_MISC_USB1_VBUS_ON	BIT(23)
#define GEMINI_MISC_USB0_MINI_B		BIT(29)
#define GEMINI_MISC_USB1_MINI_B		BIT(30)

static int fotg210_gemini_init(struct device *dev, struct resource *res)
{
	struct device_node *np = dev->of_node;
	struct regmap *map;
	bool mini_b;
	bool wakeup;
	u32 mask, val;
	int ret;

	map = syscon_regmap_lookup_by_phandle(np, "syscon");
	if (IS_ERR(map)) {
		dev_err(dev, "no syscon\n");
		return PTR_ERR(map);
	}
	mini_b = of_property_read_bool(np, "cortina,gemini-mini-b");
	wakeup = of_property_read_bool(np, "wakeup-source");

	/*
	 * Figure out if this is USB0 or USB1 by simply checking the
	 * physical base address.
	 */
	mask = 0;
	if (res->start == 0x69000000) {
		val = GEMINI_MISC_USB1_VBUS_ON;
		if (mini_b)
			val |= GEMINI_MISC_USB1_MINI_B;
		else
			mask |= GEMINI_MISC_USB1_MINI_B;
		if (wakeup)
			val |= GEMINI_MISC_USB1_WAKEUP;
		else
			mask |= GEMINI_MISC_USB1_WAKEUP;
	} else {
		val = GEMINI_MISC_USB0_VBUS_ON;
		if (mini_b)
			val |= GEMINI_MISC_USB0_MINI_B;
		else
			mask |= GEMINI_MISC_USB0_MINI_B;
		if (wakeup)
			val |= GEMINI_MISC_USB0_WAKEUP;
		else
			mask |= GEMINI_MISC_USB0_WAKEUP;
	}

	ret = regmap_update_bits(map, GEMINI_GLOBAL_MISC_CTRL, mask, val);
	if (ret) {
		dev_err(dev, "failed to initialize Gemini PHY\n");
		return ret;
	}

	dev_info(dev, "initialized Gemini PHY\n");
	return 0;
}


static int fotg210_of_probe(struct platform_device *ofdev)
{
	struct device_node *np = ofdev->dev.of_node;
	struct device *dev = &ofdev->dev;
	struct platform_device *usb_dev;
	struct fotg210_platform_data data, *pdata;
	struct fotg210_dev_data *dev_data;
	const struct of_device_id *match;
	static unsigned int idx;
	int i;

	if (!of_device_is_available(np))
		return -ENODEV;

	match = of_match_device(fotg210_dr_of_match, &ofdev->dev);
	if (!match)
		return -ENODEV;

	if (of_device_is_compatible(dev->of_node, "cortina,gemini-usb")) {
		struct resource *res;
		int retval;

		res = platform_get_resource(ofdev, IORESOURCE_MEM, 0);
		retval = fotg210_gemini_init(dev, res);
		if (retval)
			return -ENODEV;
	}

	pdata = &data;
	if (match->data)
		memcpy(pdata, match->data, sizeof(data));
	else
		memset(pdata, 0, sizeof(data));

	dev_data = get_dr_mode_data(np);

	for (i = 0; i < ARRAY_SIZE(dev_data->drivers); i++) {
		if (!dev_data->drivers[i])
			continue;
		usb_dev = fotg2_device_register(ofdev, pdata,
					dev_data->drivers[i], idx);
		if (IS_ERR(usb_dev)) {
			dev_err(&ofdev->dev, "Can't register usb device\n");
			return PTR_ERR(usb_dev);
		}
	}
	idx++;
	return 0;
}

static int __unregister_subdev(struct device *dev, void *d)
{
	platform_device_unregister(to_platform_device(dev));
	return 0;
}

static int fotg210_of_remove(struct platform_device *ofdev)
{
	device_for_each_child(&ofdev->dev, NULL, __unregister_subdev);
	return 0;
}
static const struct of_device_id fotg210_dr_of_match[] = {
	{ .compatible = "faraday,fotg210-dr" },
	{},
};
MODULE_DEVICE_TABLE(of, fotg210_dr_of_match);

static struct platform_driver fotg210_dr_driver = {
	.driver = {
		.name = "fotg210-dr",
		.of_match_table = fotg210_dr_of_match,
	},
	.probe	= fotg210_of_probe,
	.remove	= fotg210_of_remove,
};

module_platform_driver(fotg210_dr_driver);

MODULE_DESCRIPTION("FOTG210 OF device driver");
MODULE_AUTHOR("Hans Ulli Kroll <ulli.kroll@googlemail.com>");
MODULE_LICENSE("GPL");
