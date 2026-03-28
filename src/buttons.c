#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "buttons.h"

LOG_MODULE_REGISTER(BUTTONS, LOG_LEVEL_INF);

#define BUTTON_POWER_NODE DT_NODELABEL(button_power)
#define BUTTON_LEFT_NODE DT_NODELABEL(button_side_left)
#define BUTTON_RIGHT_NODE DT_NODELABEL(button_side_right)

static const struct gpio_dt_spec button_power = GPIO_DT_SPEC_GET(BUTTON_POWER_NODE, gpios);
static const struct gpio_dt_spec button_left = GPIO_DT_SPEC_GET(BUTTON_LEFT_NODE, gpios);
static const struct gpio_dt_spec button_right = GPIO_DT_SPEC_GET(BUTTON_RIGHT_NODE, gpios);

static atomic_t power_events;
static atomic_t left_events;
static atomic_t right_events;

static struct gpio_callback power_cb;
static struct gpio_callback left_cb;
static struct gpio_callback right_cb;

static void button_power_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	atomic_inc(&power_events);
}

static void button_left_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	atomic_inc(&left_events);
}

static void button_right_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	atomic_inc(&right_events);
}

int init_buttons(void)
{
	int ret;

	if (!device_is_ready(button_power.port) || !device_is_ready(button_left.port) || !device_is_ready(button_right.port))
	{
		LOG_ERR("Button GPIO device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&button_power, GPIO_INPUT);
	if (ret)
	{
		return ret;
	}
	ret = gpio_pin_configure_dt(&button_left, GPIO_INPUT);
	if (ret)
	{
		return ret;
	}
	ret = gpio_pin_configure_dt(&button_right, GPIO_INPUT);
	if (ret)
	{
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&button_power, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret)
	{
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&button_left, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret)
	{
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&button_right, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret)
	{
		return ret;
	}

	gpio_init_callback(&power_cb, button_power_pressed, BIT(button_power.pin));
	gpio_add_callback(button_power.port, &power_cb);

	gpio_init_callback(&left_cb, button_left_pressed, BIT(button_left.pin));
	gpio_add_callback(button_left.port, &left_cb);

	gpio_init_callback(&right_cb, button_right_pressed, BIT(button_right.pin));
	gpio_add_callback(button_right.port, &right_cb);

	return 0;
}

static int take_events(atomic_t *counter)
{
	int value = atomic_get(counter);

	if (value > 0)
	{
		atomic_set(counter, 0);
	}

	return value;
}

int buttons_take_power_events(void)
{
	return take_events(&power_events);
}

int buttons_take_left_events(void)
{
	return take_events(&left_events);
}

int buttons_take_right_events(void)
{
	return take_events(&right_events);
}
