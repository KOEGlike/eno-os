#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/gpio.h>

LOG_MODULE_REGISTER(MOTOR, LOG_LEVEL_DBG);

// Motor
#define MOTOR_NODE DT_ALIAS(vib_motor)
static const struct pwm_dt_spec motor = PWM_DT_SPEC_GET(MOTOR_NODE);

// Button
#define BUTTON_NODE DT_NODELABEL(button_side_left)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);

bool motor_on = false;

void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    if (motor_on)
    {
        pwm_set_dt(&motor, 0, 0); // turn off
        motor_on = false;
        LOG_INF("Motor turned OFF");
    }
    else
    {
        pwm_set_dt(&motor, 1000000, 500000); // 1ms period, 50% duty cycle
        motor_on = true;
        LOG_INF("Motor turned ON");
    }
}
static struct gpio_callback button_cb_data;

void motor_thread_func(void *arg1, void *arg2, void *arg3)
{
    int ret = 0;
    if (!device_is_ready(button.port))
    {
        LOG_ERR("Error: button device is not ready\n");
        return;
    }

    if (!pwm_is_ready_dt(&motor))
    {
        LOG_ERR("Error: motor device is not ready\n");
        return;
    }

    ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (ret < 0)
    {
        LOG_ERR("Error: button pin configuration failed\n");
        return;
    }

    ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);

    gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));

    gpio_add_callback(button.port, &button_cb_data);
}

K_THREAD_DEFINE(motor_thread_id, 1024, motor_thread_func, NULL, NULL, NULL, 7, 0, 0);
