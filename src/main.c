#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log_ctrl.h> /* LOG_PANIC(), log_panic() */
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>

// #include <lvgl.h>

LOG_MODULE_REGISTER(MAIN, LOG_LEVEL_DBG);

#define MOTOR_NODE DT_NODELABEL(motor)
static const struct gpio_dt_spec motor = GPIO_DT_SPEC_GET(MOTOR_NODE, motor_gpios);

#define AS5600_NODE DT_NODELABEL(as5600)
static const struct device *as5600 = DEVICE_DT_GET(AS5600_NODE);

// #define DISPLAY_NODE DT_CHOSEN(zephyr_display)
// static const struct device *display = DEVICE_DT_GET(DISPLAY_NODE);

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
        printk("APP: main() entered\n");

        LOG_INF("Booted up");

        int ret;

        if (!device_is_ready(motor.port))
        {
                LOG_ERR("motor.port not ready: %s", motor.port ? motor.port->name : "(null)");
                LOG_PANIC();
                return -1;
        }

        if (!device_is_ready(as5600))
        {
                LOG_ERR("AS5600 device not ready");
                return 0;
        }

        // if (!device_is_ready(display))
        // {
        //         LOG_ERR("display not ready: %s", display ? display->name : "(null)");
        //         LOG_PANIC();
        //         return -1;
        // }

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

        // lv_init();

        LOG_INF("INITIALIZED LVGL");

        // if (display_blanking_off(display))
        // {
        //         LOG_INF("Failed to turn off display blanking!");
        //         return 0;
        // }
        // LOG_INF("Display blanking is off. Screen should be cleared by full refresh.");

        turn_on_one_led(0);

        // k_msleep(2000);

        // lv_obj_t *scr = lv_scr_act();

        // // Get display width and height (for layout)
        // lv_disp_t *disp = lv_disp_get_default();
        // lv_coord_t width = lv_disp_get_hor_res(disp);
        // lv_coord_t height = lv_disp_get_ver_res(disp);
        // LOG_INF("Display width: %d, height: %d", width, height);

        // /* Make the whole screen white */
        // lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
        // lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        // // Create hello world label
        // lv_obj_t *label;
        // label = lv_label_create(scr);
        // lv_obj_set_style_text_color(label, lv_color_black(), LV_STATE_DEFAULT);
        // lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
        // lv_obj_align(label, LV_ALIGN_CENTER, 0, 5);
        // lv_label_set_text(label, "Hellooo, World!");
        // LOG_INF("Set epd text\n");

        turn_on_one_led(0);

        // Do forever
        while (1)
        {

                LOG_INF("WOKE UP ON");
                // lv_task_handler();

                int ret = sensor_sample_fetch(as5600);
                if (ret)
                {
                        LOG_ERR("sensor_sample_fetch failed: %d", ret);
                        k_msleep(1);

                        continue;
                }

                struct sensor_value angle;

                ret = sensor_channel_get(as5600, SENSOR_CHAN_ROTATION, &angle);

                if (ret)
                {
                        LOG_ERR("sensor_channel_get failed: %d", ret);
                }
                else
                {
                        /* sensor_value is fixed-point: val1 + val2*1e-6 in the channel's unit */
                        LOG_INF("Angle: %d.%06d", angle.val1, angle.val2);
                }

                k_msleep(1);
        }
}