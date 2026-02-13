#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>

LOG_MODULE_REGISTER(MAIN, LOG_LEVEL_DBG);

// Motor
#define MOTOR_NODE DT_NODELABEL(motor)
static const struct gpio_dt_spec motor = GPIO_DT_SPEC_GET(MOTOR_NODE, motor_gpios);

// PMIC
#define NPM13XX_DEVICE(dev) DEVICE_DT_GET(DT_NODELABEL(npm1300_##dev))
static const struct device *pmic = NPM13XX_DEVICE(pmic);
static const struct device *leds = NPM13XX_DEVICE(leds);
static const struct device *regulators = NPM13XX_DEVICE(regulators);
static const struct device *ldsw = NPM13XX_DEVICE(hall_pwr);

void turn_on_one_led(uint8_t led)
{
        for (uint32_t i = 0; i <= 2; i++)
        {
                led_off(leds, i);
        }
        led_on(leds, led);
}

int main(void)
{
        LOG_INF("Booted up");

        if (!device_is_ready(leds))
        {
                LOG_ERR("Error: led device is not ready\n");
                return -1;
        }
        turn_on_one_led(1);

        k_msleep(2000);

        LOG_INF("Starting audio playback");

        turn_on_one_led(0);
        while (1)
        {

                LOG_INF("WOKE UP ON");
                k_msleep(3000);
        }
}

static int init_motor_device(void)
{
        int ret;
        if (!device_is_ready(motor.port))
        {
                LOG_ERR("motor.port not ready: %s", motor.port ? motor.port->name : "(null)");
                return -1;
        }

        ret = gpio_pin_configure_dt(&motor, GPIO_OUTPUT_INACTIVE);
        if (ret < 0)
        {
                LOG_ERR("gpio_pin_configure_dt(motor) failed: %d", ret);
                return -1;
        }
}
