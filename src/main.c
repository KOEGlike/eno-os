#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log_ctrl.h> /* LOG_PANIC(), log_panic() */
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define SLEEP_TIME_MS 500

#define MOTOR_NODE DT_NODELABEL(motor)
static const struct gpio_dt_spec motor = GPIO_DT_SPEC_GET(MOTOR_NODE, motor_gpios);

#define NPM13XX_DEVICE(dev) DEVICE_DT_GET(DT_NODELABEL(npm1300_##dev))
static const struct device *pmic = NPM13XX_DEVICE(pmic);
static const struct device *leds = NPM13XX_DEVICE(leds);
static const struct device *regulators = NPM13XX_DEVICE(regulators);
static const struct device *ldsw = NPM13XX_DEVICE(hall_pwr);

int main(void)
{
        LOG_INF("Booted up");
        printk("XD\r\n");

        int ret;

        if (!device_is_ready(motor.port))
        {
                LOG_ERR("motor.port not ready: %s", motor.port ? motor.port->name : "(null)");
                LOG_PANIC();
                return -1;
        }

        ret = gpio_pin_configure_dt(&motor, GPIO_OUTPUT_INACTIVE);
        if (ret < 0)
        {
                LOG_ERR("gpio_pin_configure_dt(motor) failed: %d", ret);
                LOG_PANIC();
                return -1;
        }

        if (!device_is_ready(leds))
        {
                LOG_ERR("Error: led device is not ready\n");
                return -1;
        }

        // turn off all of the LEDs
        for (uint32_t i = 0; i <= 2; i++)
        {
                led_off(leds, i);
        }

        // Do forever
        while (1)
        {

                LOG_INF("WOKE UP ON");
                led_on(leds, 0U);
                k_msleep(SLEEP_TIME_MS);

                LOG_INF("WOKE UP OFF");
                led_off(leds, 0U);
                k_msleep(SLEEP_TIME_MS);
        }
}