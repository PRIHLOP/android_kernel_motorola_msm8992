/*
 * TI LM3633 LED driver
 *
 * Copyright 2014 Texas Instruments
 *
 * Author: Milo Kim <milo.kim@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * LM3633 LED driver has features below.
 *
 *   - Generic LED subsystem control
 *   - LED string configuration
 *   - Pattern programming via the sysfs
 *   - Platform data configuration from the device tree nodes
 *
 * Pattern generated by using LMU effect driver APIs.
 *
 */

#include <linux/leds.h>
#include <linux/mfd/ti-lmu.h>
#include <linux/mfd/ti-lmu-effect.h>
#include <linux/mfd/ti-lmu-register.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define LM3633_LED_MAX_BRIGHTNESS		255
#define LM3633_DEFAULT_LED_NAME			"indicator"

enum lm3633_led_bank_id {
	LM3633_LED_BANK_C,
	LM3633_LED_BANK_D,
	LM3633_LED_BANK_E,
	LM3633_LED_BANK_F,
	LM3633_LED_BANK_G,
	LM3633_LED_BANK_H,
	LM3633_MAX_LEDS,
};

struct lm3633_pattern_time {
	unsigned int delay;
	unsigned int rise;
	unsigned int high;
	unsigned int fall;
	unsigned int low;
};

struct lm3633_pattern_level {
	u8 low;
	u8 high;
};

/* One LED chip can have multiple LED strings (max: 6) */
struct ti_lmu_led_chip {
	struct device *dev;
	struct ti_lmu *lmu;
	struct mutex lock;
	int num_leds;
};

/* LED string structure */
struct ti_lmu_led {
	enum lm3633_led_bank_id bank_id;

	struct led_classdev cdev;
	struct ti_lmu_led_chip *chip;
	struct ti_lmu_led_platform_data *led_pdata;

	struct work_struct work;
	enum led_brightness brightness;

	/* Pattern specific data */
	struct lm3633_pattern_time time;
	struct lm3633_pattern_level level;
};

static struct ti_lmu_led *to_ti_lmu_led(struct device *dev)
{
	struct led_classdev *_cdev = dev_get_drvdata(dev);
	return container_of(_cdev, struct ti_lmu_led, cdev);
}

static int lm3633_led_config_bank(struct ti_lmu_led *lmu_led)
{
	u8 val;
	int i, ret;
	u8 group_led[] = { 0, BIT(0), BIT(0), 0, BIT(3), BIT(3), };
	enum lm3633_led_bank_id default_id[] = {
		LM3633_LED_BANK_C, LM3633_LED_BANK_C, LM3633_LED_BANK_C,
		LM3633_LED_BANK_F, LM3633_LED_BANK_F, LM3633_LED_BANK_F,
	};
	enum lm3633_led_bank_id separate_id[] = {
		LM3633_LED_BANK_C, LM3633_LED_BANK_D, LM3633_LED_BANK_E,
		LM3633_LED_BANK_F, LM3633_LED_BANK_G, LM3633_LED_BANK_H,
	};

	/*
	 * Check configured LED string and assign control bank
	 *
	 * Each LED is tied with other LEDS (group):
	 *   the default control bank is assigned
	 *
	 * Otherwise:
	 *   separate bank is assigned
	 */

	for (i = 0; i < LM3633_MAX_LEDS; i++) {
		/* LED 1 and LED 4 are fixed, so no assignment is required */
		if (i == 0 || i == 3)
			continue;

		if (test_bit(i, &lmu_led->led_pdata->led_string)) {
			if (lmu_led->led_pdata->led_string & group_led[i]) {
				lmu_led->bank_id = default_id[i];
				val = 0;
			} else {
				lmu_led->bank_id = separate_id[i];
				val = BIT(i);
			}

			ret = ti_lmu_update_bits(lmu_led->chip->lmu,
						 LM3633_REG_BANK_SEL,
						 BIT(i), val);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int lm3633_led_enable_bank(struct ti_lmu_led *lmu_led, bool on)
{
	u8 mask = 1 << (lmu_led->bank_id + LM3633_LED_BANK_OFFSET);

	if (on)
		return ti_lmu_update_bits(lmu_led->chip->lmu,
					  LM3633_REG_ENABLE, mask, mask);
	else
		return ti_lmu_update_bits(lmu_led->chip->lmu,
					  LM3633_REG_ENABLE, mask, 0);
}

/*
 * This callback function is invoked in case the LMU effect driver is
 * requested successfully.
 */
static void lm3633_led_effect_cb(struct ti_lmu_effect *lmu_effect,
				 int req_id, void *data)
{
	struct ti_lmu_led *lmu_led = data;
	u8 reg_offset = lmu_led->bank_id * LM3633_PATTERN_REG_OFFSET;

	switch (req_id) {
	case LED_EFFECT_DELAY:
		ti_lmu_effect_set_time(lmu_effect, lmu_led->time.delay,
				       reg_offset);
		break;
	case LED_EFFECT_HIGHTIME:
		ti_lmu_effect_set_time(lmu_effect, lmu_led->time.high,
				       reg_offset);
		break;
	case LED_EFFECT_LOWTIME:
		ti_lmu_effect_set_time(lmu_effect, lmu_led->time.low,
				       reg_offset);
		break;
	case LED_EFFECT_PTN0_RAMPUP:
	case LED_EFFECT_PTN1_RAMPUP:
		ti_lmu_effect_set_ramp(lmu_effect, lmu_led->time.rise);
		break;
	case LED_EFFECT_PTN0_RAMPDN:
	case LED_EFFECT_PTN1_RAMPDN:
		ti_lmu_effect_set_ramp(lmu_effect, lmu_led->time.fall);
		break;
	case LED_EFFECT_LOWBRT:
		ti_lmu_effect_set_level(lmu_effect, lmu_led->level.low,
					reg_offset);
		break;
	case LED_EFFECT_HIGHBRT:
		ti_lmu_effect_set_level(lmu_effect, lmu_led->level.high,
					lmu_led->bank_id);
		break;
	default:
		break;
	}
}

static int lm3633_led_effect_request(enum lmu_effect_request_id id,
				     struct ti_lmu_led *lmu_led)
{
	const char *name[] = {
		[LED_EFFECT_DELAY]       = LM3633_EFFECT_PTN_DELAY,
		[LED_EFFECT_HIGHTIME]    = LM3633_EFFECT_PTN_HIGHTIME,
		[LED_EFFECT_LOWTIME]     = LM3633_EFFECT_PTN_LOWTIME,
		[LED_EFFECT_PTN0_RAMPUP] = LM3633_EFFECT_PTN0_RAMPUP,
		[LED_EFFECT_PTN0_RAMPDN] = LM3633_EFFECT_PTN0_RAMPDOWN,
		[LED_EFFECT_PTN1_RAMPUP] = LM3633_EFFECT_PTN1_RAMPUP,
		[LED_EFFECT_PTN1_RAMPDN] = LM3633_EFFECT_PTN1_RAMPDOWN,
		[LED_EFFECT_LOWBRT]      = LM3633_EFFECT_PTN_LOWBRT,
		[LED_EFFECT_HIGHBRT]     = LM3633_EFFECT_PTN_HIGHBRT,
	};

	return ti_lmu_effect_request(name[id], lm3633_led_effect_cb, id,
				     lmu_led);
}

static ssize_t lm3633_led_show_pattern_times(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct ti_lmu_led *lmu_led = to_ti_lmu_led(dev);

	return sprintf(buf, "delay: %u, rise: %u, high:%u, fall:%u, low: %u\n",
		       lmu_led->time.delay, lmu_led->time.rise,
		       lmu_led->time.high, lmu_led->time.fall,
		       lmu_led->time.low);
}

static ssize_t lm3633_led_store_pattern_times(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t len)
{
	struct ti_lmu_led *lmu_led = to_ti_lmu_led(dev);
	int ret;

	/*
	 * Sequence
	 *
	 * 1) Read pattern time data (unit: msec)
	 * 2) Update DELAY register
	 * 3) Update HIGH TIME register
	 * 4) Update LOW TIME register
	 * 5) Update RAMP TIME register
	 */

	ret = sscanf(buf, "%u %u %u %u %u", &lmu_led->time.delay,
		     &lmu_led->time.rise, &lmu_led->time.high,
		     &lmu_led->time.fall, &lmu_led->time.low);
	if (ret != 5)
		return -EINVAL;

	mutex_lock(&lmu_led->chip->lock);

	ret = lm3633_led_effect_request(LED_EFFECT_DELAY, lmu_led);
	if (ret)
		goto skip;

	ret = lm3633_led_effect_request(LED_EFFECT_HIGHTIME, lmu_led);
	if (ret)
		goto skip;

	ret = lm3633_led_effect_request(LED_EFFECT_LOWTIME, lmu_led);
	if (ret)
		goto skip;

	switch (lmu_led->bank_id) {
	case LM3633_LED_BANK_C:
	case LM3633_LED_BANK_D:
	case LM3633_LED_BANK_E:
		ret = lm3633_led_effect_request(LED_EFFECT_PTN0_RAMPUP,
						lmu_led);
		if (ret)
			goto skip;

		ret = lm3633_led_effect_request(LED_EFFECT_PTN0_RAMPDN,
						lmu_led);
		if (ret)
			goto skip;
		break;
	case LM3633_LED_BANK_F:
	case LM3633_LED_BANK_G:
	case LM3633_LED_BANK_H:
		ret = lm3633_led_effect_request(LED_EFFECT_PTN1_RAMPUP,
						lmu_led);
		if (ret)
			goto skip;

		ret = lm3633_led_effect_request(LED_EFFECT_PTN1_RAMPDN,
						lmu_led);
		if (ret)
			goto skip;
		break;
	default:
		break;
	}

	mutex_unlock(&lmu_led->chip->lock);
	return len;
skip:
	mutex_unlock(&lmu_led->chip->lock);
	return ret;
}

static ssize_t lm3633_led_show_pattern_levels(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct ti_lmu_led *lmu_led = to_ti_lmu_led(dev);

	return sprintf(buf, "low brightness: %u, high brightness: %u\n",
		       lmu_led->level.low, lmu_led->level.high);
}

static ssize_t lm3633_led_store_pattern_levels(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t len)
{
	struct ti_lmu_led *lmu_led = to_ti_lmu_led(dev);
	unsigned int low, high;
	int ret;

	/*
	 * Sequence
	 *
	 * 1) Read pattern level data
	 * 2) Disable a bank before programming a pattern
	 * 3) Update LOW BRIGHTNESS register
	 * 4) Update HIGH BRIGHTNESS register
	 * 5) Update PATTERN ENABLE register
	 * 6) Enable the bank if required
	 */

	ret = sscanf(buf, "%u %u", &low, &high);
	if (ret != 2)
		return -EINVAL;

	low = min_t(unsigned int, low, LM3633_LED_MAX_BRIGHTNESS);
	high = min_t(unsigned int, high, LM3633_LED_MAX_BRIGHTNESS);
	lmu_led->level.low = (u8)low;
	lmu_led->level.high = (u8)high;

	mutex_lock(&lmu_led->chip->lock);
	ret = lm3633_led_enable_bank(lmu_led, false);
	if (ret)
		goto skip;

	ret = lm3633_led_effect_request(LED_EFFECT_LOWBRT, lmu_led);
	if (ret)
		goto skip;

	ret = lm3633_led_effect_request(LED_EFFECT_HIGHBRT, lmu_led);
	if (ret)
		goto skip;

	mutex_unlock(&lmu_led->chip->lock);
	return len;
skip:
	mutex_unlock(&lmu_led->chip->lock);
	return ret;
}

static ssize_t lm3633_led_run_pattern(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t len)
{
	struct ti_lmu_led *lmu_led = to_ti_lmu_led(dev);
	struct ti_lmu_led_chip *chip = lmu_led->chip;
	u8 offset = lmu_led->bank_id + LM3633_LED_BANK_OFFSET;
	u8 mask = LM3633_PATTERN_EN << offset;
	unsigned long enable;
	int ret;

	if (kstrtoul(buf, 0, &enable))
		return -EINVAL;

	mutex_lock(&chip->lock);
	if (enable)
		ret = ti_lmu_update_bits(chip->lmu, LM3633_REG_PATTERN, mask,
					 mask);
	else
		ret = ti_lmu_update_bits(chip->lmu, LM3633_REG_PATTERN, mask,
					 0);
	if (ret) {
		mutex_unlock(&chip->lock);
		return ret;
	}

	if (enable)
		lm3633_led_enable_bank(lmu_led, true);

	mutex_unlock(&chip->lock);

	return len;
}

static DEVICE_ATTR(pattern_times, S_IRUGO | S_IWUSR,
		   lm3633_led_show_pattern_times,
		   lm3633_led_store_pattern_times);
static DEVICE_ATTR(pattern_levels, S_IRUGO | S_IWUSR,
		   lm3633_led_show_pattern_levels,
		   lm3633_led_store_pattern_levels);
static DEVICE_ATTR(run_pattern, S_IWUSR, NULL,
		   lm3633_led_run_pattern);

static struct attribute *lm3633_led_attributes[] = {
	&dev_attr_pattern_times.attr,
	&dev_attr_pattern_levels.attr,
	&dev_attr_run_pattern.attr,
	NULL,
};

static struct attribute_group lm3633_led_attr_group = {
	.attrs = lm3633_led_attributes
};

static int lm3633_led_set_max_current(struct ti_lmu_led *lmu_led)
{
	u8 reg = LM3633_REG_IMAX_LVLED_BASE + lmu_led->bank_id;
	enum ti_lmu_max_current imax = lmu_led->led_pdata->imax;

	return ti_lmu_write_byte(lmu_led->chip->lmu, reg, imax);
}

static void lm3633_led_work(struct work_struct *_work)
{
	struct ti_lmu_led *lmu_led;
	struct ti_lmu_led_chip *chip;

	lmu_led = container_of(_work, struct ti_lmu_led, work);
	chip = lmu_led->chip;

	mutex_lock(&chip->lock);

	ti_lmu_write_byte(chip->lmu,
			  LM3633_REG_BRT_LVLED_BASE + lmu_led->bank_id,
			  lmu_led->brightness);

	if (lmu_led->brightness == 0)
		lm3633_led_enable_bank(lmu_led, false);
	else
		lm3633_led_enable_bank(lmu_led, true);

	mutex_unlock(&chip->lock);
}

static void lm3633_led_brightness_set(struct led_classdev *_cdev,
				      enum led_brightness brt_val)
{
	struct ti_lmu_led *lmu_led;

	lmu_led = container_of(_cdev, struct ti_lmu_led, cdev);
	lmu_led->brightness = brt_val;

	schedule_work(&lmu_led->work);
}

static int lm3633_led_init(struct ti_lmu_led *lmu_led, int bank_id)
{
	struct device *dev = lmu_led->chip->dev;
	char name[12];
	int ret;

	/*
	 * Sequence
	 *
	 * 1) Configure LED bank which is used for brightness control
	 * 2) Set max current for each output channel
	 * 3) Add LED device
	 * 4) Add sysfs attributes for LED pattern
	 */

	ret = lm3633_led_config_bank(lmu_led);
	if (ret) {
		dev_err(dev, "Output bank register err: %d\n", ret);
		return ret;
	}

	ret = lm3633_led_set_max_current(lmu_led);
	if (ret) {
		dev_err(dev, "Set max current err: %d\n", ret);
		return ret;
	}

	lmu_led->cdev.max_brightness = LM3633_LED_MAX_BRIGHTNESS;
	lmu_led->cdev.brightness_set = lm3633_led_brightness_set;
	if (lmu_led->led_pdata->name) {
		lmu_led->cdev.name = lmu_led->led_pdata->name;
	} else {
		snprintf(name, sizeof(name), "%s:%d", LM3633_DEFAULT_LED_NAME,
			 bank_id);
		lmu_led->cdev.name = name;
	}

	ret = led_classdev_register(dev, &lmu_led->cdev);
	if (ret) {
		dev_err(dev, "LED register err: %d\n", ret);
		return ret;
	}

	ret = sysfs_create_group(&lmu_led->cdev.dev->kobj,
				 &lm3633_led_attr_group);
	if (ret) {
		dev_err(dev, "LED sysfs err: %d\n", ret);
		return ret;
	}

	INIT_WORK(&lmu_led->work, lm3633_led_work);

	return 0;
}

static int lm3633_led_parse_dt(struct device *dev, struct ti_lmu *lmu)
{
	struct ti_lmu_led_platform_data *pdata;
	struct device_node *node = dev->of_node;
	struct device_node *child;
	int num_leds;
	int i = 0;
	u8 imax_mA;

	if (!node) {
		dev_err(dev, "No device node exists\n");
		return -ENODEV;
	}

	num_leds = of_get_child_count(node);
	if (num_leds == 0) {
		dev_err(dev, "No LED channels\n");
		return -EINVAL;
	}

	pdata = devm_kzalloc(dev, sizeof(*pdata) * num_leds, GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	for_each_child_of_node(node, child) {
		of_property_read_string(child, "chan-name", &pdata[i].name);

		/* Make LED strings */
		pdata[i].led_string = 0;
		if (of_find_property(child, "lvled1-used", NULL))
			pdata[i].led_string |= LMU_LVLED1;
		if (of_find_property(child, "lvled2-used", NULL))
			pdata[i].led_string |= LMU_LVLED2;
		if (of_find_property(child, "lvled3-used", NULL))
			pdata[i].led_string |= LMU_LVLED3;
		if (of_find_property(child, "lvled4-used", NULL))
			pdata[i].led_string |= LMU_LVLED4;
		if (of_find_property(child, "lvled5-used", NULL))
			pdata[i].led_string |= LMU_LVLED5;
		if (of_find_property(child, "lvled6-used", NULL))
			pdata[i].led_string |= LMU_LVLED6;

		of_property_read_u8(child, "max-current-milliamp", &imax_mA);
		pdata[i].imax = ti_lmu_get_current_code(imax_mA);

		i++;
	}

	lmu->pdata->led_pdata = pdata;
	lmu->pdata->num_leds = num_leds;

	return 0;
}

static int lm3633_led_probe(struct platform_device *pdev)
{
	struct ti_lmu *lmu = dev_get_drvdata(pdev->dev.parent);
	struct ti_lmu_led_chip *chip;
	struct ti_lmu_led *lmu_led, *each;
	struct device *dev = &pdev->dev;
	int i, ret, num_leds;

	if (!lmu->pdata->led_pdata) {
		if (IS_ENABLED(CONFIG_OF))
			ret = lm3633_led_parse_dt(dev, lmu);
		else
			return -ENODEV;

		if (ret)
			return ret;
	}

	num_leds = lmu->pdata->num_leds;
	if (num_leds > LM3633_MAX_LEDS || num_leds <= 0) {
		dev_err(dev, "Invalid num_leds: %d\n", num_leds);
		return -EINVAL;
	}

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = dev;
	chip->lmu = lmu;

	lmu_led = devm_kzalloc(dev, sizeof(*lmu_led) * num_leds, GFP_KERNEL);
	if (!lmu_led)
		return -ENOMEM;

	for (i = 0; i < num_leds; i++) {
		each = lmu_led + i;
		each->bank_id = 0;
		each->chip = chip;
		each->led_pdata = lmu->pdata->led_pdata + i;

		ret = lm3633_led_init(each, i);
		if (ret) {
			dev_err(dev, "Initialize a LED err: %d\n", ret);
			goto cleanup_leds;
		}
	}

	chip->num_leds = num_leds;
	mutex_init(&chip->lock);
	platform_set_drvdata(pdev, lmu_led);

	return 0;

cleanup_leds:
	while (--i >= 0) {
		each = lmu_led + i;
		led_classdev_unregister(&each->cdev);
	}
	return ret;
}

static int lm3633_led_remove(struct platform_device *pdev)
{
	struct ti_lmu_led *lmu_led = platform_get_drvdata(pdev);
	struct ti_lmu_led *each;
	int i;

	for (i = 0; i < lmu_led->chip->num_leds; i++) {
		each = lmu_led + i;
		led_classdev_unregister(&each->cdev);
		flush_work(&each->work);
	}

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id lm3633_led_of_match[] = {
	{ .compatible = "ti,lm3633-leds", },
	{ }
};
MODULE_DEVICE_TABLE(of, lm3633_led_of_match);
#endif

static struct platform_driver lm3633_led_driver = {
	.probe = lm3633_led_probe,
	.remove = lm3633_led_remove,
	.driver = {
		.name = "lm3633-leds",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(lm3633_led_of_match),
	},
};
module_platform_driver(lm3633_led_driver);

MODULE_DESCRIPTION("TI LM3633 LED Driver");
MODULE_AUTHOR("Milo Kim");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:lm3633-leds");
