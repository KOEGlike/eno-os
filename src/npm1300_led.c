#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>

LOG_MODULE_REGISTER(NPM1300_LED, LOG_LEVEL_DBG);

// PMIC
#define NPM13XX_DEVICE(dev) DEVICE_DT_GET(DT_NODELABEL(npm1300_##dev))
static const struct device *pmic = NPM13XX_DEVICE(pmic);
static const struct device *leds = NPM13XX_DEVICE(leds);
static const struct device *regulators = NPM13XX_DEVICE(regulators);
static const struct device *hall_ldsw = NPM13XX_DEVICE(hall_pwr);

void turn_on_one_led(uint8_t led)
{
    for (uint32_t i = 0; i <= 2; i++)
    {
        led_off(leds, i);
    }
    led_on(leds, led);
}

int npm1300_led__thread_func(void)
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

K_THREAD_DEFINE(npm1300_led_thread, 1024, npm1300_led__thread_func, NULL, NULL, NULL, 5, 0, 0);
