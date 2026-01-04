#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h> /* LOG_PANIC(), log_panic() */

// #include <lvgl.h>
#include <zephyr/drivers/display.h>

LOG_MODULE_REGISTER(MAIN, LOG_LEVEL_DBG);

#define SLEEP_TIME_MS 2000

#define SW0_NODE DT_NODELABEL(button_side_right)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);

#define MOTOR_NODE DT_NODELABEL(motor)
static const struct gpio_dt_spec motor = GPIO_DT_SPEC_GET(MOTOR_NODE, motor_gpios);

// #define DISPLAY_NODE DT_CHOSEN(zephyr_display)
// static const struct device *display = DEVICE_DT_GET(DISPLAY_NODE);

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

        // if (!device_is_ready(display))
        // {
        //         LOG_ERR("display not ready: %s", display ? display->name : "(null)");
        //         LOG_PANIC();
        //         return -1;
        // }

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

        // if (display_blanking_off(display))
        // {
        //         LOG_INF("Failed to turn off display blanking!");
        //         return 0;
        // }
        // LOG_INF("Display blanking is off. Screen should be cleared by full refresh.");

        //        k_msleep(2000);

        // lv_obj_t *scr = lv_scr_act();

        // // Get display width and height (for layout)
        // lv_disp_t *disp = lv_disp_get_default();
        // lv_coord_t width = lv_disp_get_hor_res(disp);
        // lv_coord_t height = lv_disp_get_ver_res(disp);
        // LOG_INF("Display width: %d, height: %d", width, height);

        // LOG_INF("\n\ngoing to sleep\r\n");

        // k_msleep(20000);

        // LOG_INF("woke up from long sssss\r\n");

        // lv_obj_t *label;
        // label = lv_label_create(scr);
        // lv_obj_set_style_text_color(label, lv_color_black(), LV_STATE_DEFAULT);
        // lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 5);
        // lv_label_set_text(label, "Hello, World!");
        // LOG_INF("Set epd text\n");

        // Do forever
        while (1)
        {
                // // Update counter label every second
                // count++;
                // if ((count % (1000 / SLEEP_TIME_MS)) == 0)
                // {
                //         sprintf(buf, "%d", count / (1000 / SLEEP_TIME_MS));
                //         lv_label_set_text(label, buf);
                // }

                LOG_INF("WOKE UP\r\n");

                // Must be called periodically
                // lv_task_handler();

                // Sleep
                k_msleep(SLEEP_TIME_MS);
        }
}