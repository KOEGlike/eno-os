#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h> /* LOG_PANIC(), log_panic() */
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>

#include <lvgl.h>

LOG_MODULE_REGISTER(MAIN, LOG_LEVEL_DBG);

#define SLEEP_TIME_MS 200

#define MOTOR_NODE DT_NODELABEL(motor)
static const struct gpio_dt_spec motor = GPIO_DT_SPEC_GET(MOTOR_NODE, motor_gpios);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
static const struct device *display = DEVICE_DT_GET(DISPLAY_NODE);

#define NPM13XX_DEVICE(dev) DEVICE_DT_GET(DT_NODELABEL(npm1300_##dev))
static const struct device *pmic = NPM13XX_DEVICE(pmic);
static const struct device *leds = NPM13XX_DEVICE(leds);
static const struct device *regulators = NPM13XX_DEVICE(regulators);
static const struct device *ldsw = NPM13XX_DEVICE(hall_pwr);

int main(void)
{
        printk("APP: main() entered\n");

        LOG_DBG("Booted up");

        int ret;

        if (!device_is_ready(motor.port))
        {
                LOG_ERR("motor.port not ready: %s", motor.port ? motor.port->name : "(null)");
                LOG_PANIC();
                return -1;
        }

        if (!device_is_ready(display))
        {
                LOG_ERR("display not ready: %s", display ? display->name : "(null)");
                LOG_PANIC();
                return -1;
        }

        if (!device_is_ready(leds))
        {
                LOG_ERR("Error: led device is not ready\n");
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

        lv_init();

        LOG_DBG("INITIALIZED LVGL");

        if (display_blanking_off(display))
        {
                LOG_DBG("Failed to turn off display blanking!");
                return 0;
        }
        LOG_DBG("Display blanking is off. Screen should be cleared by full refresh.");

        for (uint32_t i = 0; i <= 2; i++)
        {
                led_off(leds, i);
        }
        led_on(leds, 0U);

        k_msleep(2000);

        lv_obj_t *scr = lv_scr_act();

        // // Get display width and height (for layout)
        // lv_disp_t *disp = lv_disp_get_default();
        // lv_coord_t width = lv_disp_get_hor_res(disp);
        // lv_coord_t height = lv_disp_get_ver_res(disp);
        // LOG_DBG("Display width: %d, height: %d", width, height);

        // LOG_DBG("\n\ngoing to sleep");

        // k_msleep(20000);

        // LOG_DBG("woke up from long sssss");

        lv_obj_t *label;
        label = lv_label_create(scr);
        lv_obj_set_style_text_color(label, lv_color_black(), LV_STATE_DEFAULT);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 5);
        lv_label_set_text(label, "Hello, World!");
        LOG_DBG("Set epd text\n");

        for (uint32_t i = 0; i <= 2; i++)
        {
                led_off(leds, i);
        }
        led_on(leds, 2U);

        // Do forever
        while (1)
        {
                LOG_DBG("WOKE UP");

                // Must be called periodically
                lv_task_handler();

                // Sleep
                k_msleep(SLEEP_TIME_MS);
        }
}