#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "buttons.h"

LOG_MODULE_REGISTER(BUTTONS, LOG_LEVEL_INF);

#define BUTTON_POWER_NODE DT_NODELABEL(button_power)
#define BUTTON_LEFT_NODE DT_NODELABEL(button_side_left)
#define BUTTON_RIGHT_NODE DT_NODELABEL(button_side_right)

/* Edges are not trusted directly: after an edge, the line must stay
 * settled this long before the pin level is sampled to decide what
 * happened. A timestamp-only debounce let double-make presses through
 * (the second make arrives well after the bounce window); confirming
 * the level and re-arming only on a confirmed release collapses any
 * number of make-breaks into exactly one event per press.
 */
#define BUTTON_CONFIRM_DELAY_MS 30

struct button {
	const struct gpio_dt_spec spec;
	struct gpio_callback cb;
	struct k_work_delayable confirm_work;
	atomic_t waiting_release;
	atomic_t events;
};

static struct button button_power = {
	.spec = GPIO_DT_SPEC_GET(BUTTON_POWER_NODE, gpios),
};
static struct button button_left = {
	.spec = GPIO_DT_SPEC_GET(BUTTON_LEFT_NODE, gpios),
};
static struct button button_right = {
	.spec = GPIO_DT_SPEC_GET(BUTTON_RIGHT_NODE, gpios),
};

/* Runs on the system workqueue, BUTTON_CONFIRM_DELAY_MS after the
 * last edge: the line has had time to settle, so its level now tells
 * press (count, wait for release) from glitch (ignore).
 */
static void button_confirm(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct button *btn = CONTAINER_OF(dwork, struct button, confirm_work);
	bool active = gpio_pin_get_dt(&btn->spec) == 1;

	if (!active)
	{
		/* Settled at inactive: re-arm for the next press */
		atomic_set(&btn->waiting_release, 0);
		return;
	}

	if (atomic_cas(&btn->waiting_release, 0, 1))
	{
		atomic_inc(&btn->events);
	}
}

static void button_edge(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(pins);

	struct button *btn = CONTAINER_OF(cb, struct button, cb);
	bool active = gpio_pin_get_dt(&btn->spec) == 1;

	if (active && atomic_get(&btn->waiting_release))
	{
		/* Still held (double-make, jitter): a second press is
		 * only accepted after a confirmed release
		 */
		return;
	}

	/* Every edge pushes the confirmation out, so the level is
	 * sampled once the line has stopped bouncing
	 */
	k_work_reschedule(&btn->confirm_work, K_MSEC(BUTTON_CONFIRM_DELAY_MS));
}

int init_buttons(void)
{
	static struct button *const buttons[] = {
		&button_power,
		&button_left,
		&button_right,
	};
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++)
	{
		struct button *btn = buttons[i];

		if (!device_is_ready(btn->spec.port))
		{
			LOG_ERR("Button GPIO device not ready");
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&btn->spec, GPIO_INPUT);
		if (ret)
		{
			return ret;
		}

		/* Both edges: releases end the hold-off so the next
		 * press is accepted
		 */
		ret = gpio_pin_interrupt_configure_dt(&btn->spec, GPIO_INT_EDGE_BOTH);
		if (ret)
		{
			return ret;
		}

		k_work_init_delayable(&btn->confirm_work, button_confirm);

		gpio_init_callback(&btn->cb, button_edge, BIT(btn->spec.pin));
		gpio_add_callback(btn->spec.port, &btn->cb);
	}

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
	return take_events(&button_power.events);
}

int buttons_take_left_events(void)
{
	return take_events(&button_left.events);
}

int buttons_take_right_events(void)
{
	return take_events(&button_right.events);
}
