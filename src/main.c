#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>
#include <zephyr/drivers/display.h>

LOG_MODULE_REGISTER(MAIN);

#define SLEEP_TIME_MS 10 * 60 * 1000

#define SW0_NODE DT_NODELABEL(button_side_right)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);

#define MOTOR_NODE DT_NODELABEL(motor)
static const struct gpio_dt_spec motor = GPIO_DT_SPEC_GET(MOTOR_NODE, motor_gpios);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
static const struct device *display = DEVICE_DT_GET(DISPLAY_NODE);

void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
        printk("toggled motor");
        gpio_pin_toggle_dt(&motor);
}
static struct gpio_callback button_cb_data;

int main(void)
{
        LOG_INF("Booted up");
        printk("testing");
        int ret;

        if (!device_is_ready(motor.port))
        {
                return -1;
        }

        if (!device_is_ready(button.port))
        {
                return -1;
        }

        if (!device_is_ready(display))
        {
                printk("Error setting up display\r\n");
                return -1;
        }

        ret = gpio_pin_configure_dt(&motor, GPIO_OUTPUT_ACTIVE);
        if (ret < 0)
        {
                return -1;
        }

        ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
        if (ret < 0)
        {
                return -1;
        }
        ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);

        gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
        gpio_add_callback(button.port, &button_cb_data);

        lv_obj_t *label;
        label = lv_label_create(lv_scr_act());
        lv_label_set_text(label, "Hello world!");
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

        while (1)
        {
                /* STEP 8 - Remove the polling code */

                k_msleep(SLEEP_TIME_MS);
        }
}