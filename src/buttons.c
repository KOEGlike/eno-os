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

/* The DT debounce-interval only applies to the gpio-keys subsystem,
 * which this raw GPIO driver does not use — bounce filtering has to
 * happen here. A mechanical switch settles in <10 ms; 100 ms also
 * rate-limits the navigation buttons comfortably.
 */
#define BUTTON_DEBOUNCE_MS 100

static int64_t power_last_ms;
static int64_t left_last_ms;
static int64_t right_last_ms;

static bool button_debounce(int64_t *last_ms)
{
	int64_t now = k_uptime_get();

	if (now - *last_ms < BUTTON_DEBOUNCE_MS)
	{
		return false;
	}
	*last_ms = now;
	return true;
}

static void button_power_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (!button_debounce(&power_last_ms))
	{
		return;
	}
	atomic_inc(&power_events);
}

static void button_left_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (!button_debounce(&left_last_ms))
	{
		return;
	}
	atomic_inc(&left_events);
}

static void button_right_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (!button_debounce(&right_last_ms))
	{
		return;
	}
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
