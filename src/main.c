#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h> /* LOG_PANIC(), log_panic() */

LOG_MODULE_REGISTER(MAIN, LOG_LEVEL_DBG);

#define SLEEP_TIME_MS 2000

#define SW0_NODE DT_NODELABEL(button_side_right)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);

#define MOTOR_NODE DT_NODELABEL(motor)
static const struct gpio_dt_spec motor = GPIO_DT_SPEC_GET(MOTOR_NODE, motor_gpios);

void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
        LOG_INF("toggled motor\r\n");
        gpio_pin_toggle_dt(&motor);
}
static struct gpio_callback button_cb_data;

int main(void)
{
        LOG_INF("Booted up\r\n");

        int ret;

        if (!device_is_ready(motor.port))
        {
                LOG_ERR("motor.port not ready: %s", motor.port ? motor.port->name : "(null)");
                LOG_PANIC();
                return -1;
        }

        if (!device_is_ready(button.port))
        {
                LOG_ERR("button.port not ready: %s", button.port ? button.port->name : "(null)");
                LOG_PANIC();
                return -1;
        }

        ret = gpio_pin_configure_dt(&motor, GPIO_OUTPUT_ACTIVE);
        if (ret < 0)
        {
                LOG_ERR("gpio_pin_configure_dt(motor) failed: %d", ret);
                LOG_PANIC();
                return -1;
        }

        ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
        if (ret < 0)
        {
                LOG_ERR("gpio_pin_configure_dt(button) failed: %d", ret);
                LOG_PANIC();
                return -1;
        }
        ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
        if (ret < 0)
        {
                LOG_ERR("gpio_pin_interrupt_configure_dt(button) failed: %d", ret);
                LOG_PANIC();
                return -1;
        }

        gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
        ret = gpio_add_callback(button.port, &button_cb_data);
        if (ret < 0)
        {
                LOG_ERR("gpio_add_callback failed: %d", ret);
                LOG_PANIC();
                return -1;
        }

        // lv_init();

        LOG_INF("idididd");
        LOG_INF("INITIALIZED LVGL\r\n");

        // Do forever
        while (1)
        {

                LOG_INF("WOKE UP\r\n");

                // Sleep
                k_msleep(SLEEP_TIME_MS);
        }
}