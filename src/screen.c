#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/display.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>

LOG_MODULE_REGISTER(SCREEN, LOG_LEVEL_DBG);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
static const struct device *display = DEVICE_DT_GET(DISPLAY_NODE);

void update_screen_thread_func(void *arg1, void *arg2, void *arg3)
{
    if (!device_is_ready(display))
    {
        LOG_ERR("display not ready: %s", display ? display->name : "(null)");
        return -1;
    }

    lv_init();

    k_msleep(4000); // Give some time for the display to be ready before initializing LVGL

    LOG_INF("INITIALIZED LVGL");

    if (display_blanking_off(display))
    {
        LOG_INF("Failed to turn off display blanking!");
        return 0;
    }
    LOG_INF("Display blanking is off. Screen should be cleared by full refresh.");

    lv_obj_t *scr = lv_scr_act();

    // Get display width and height (for layout)
    lv_disp_t *disp = lv_disp_get_default();
    lv_coord_t width = lv_disp_get_hor_res(disp);
    lv_coord_t height = lv_disp_get_ver_res(disp);
    LOG_INF("Display width: %d, height: %d", width, height);

    /* Make the whole screen white */
    lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    // Create hello world label
    lv_obj_t *label;
    label = lv_label_create(scr);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 5);
    lv_label_set_text(label, "Hellooo, World!");
    LOG_INF("Set epd text\n");

    int counter = 0;
    char buf[64];
    while (1)
    {
        snprintf(buf, sizeof(buf), "Hellooo, World!\n Counter: %d", counter++);
        lvgl_lock();
        lv_label_set_text(label, buf);
        lvgl_unlock();
        LOG_INF("Updated label text: %s", buf);
        k_msleep(1000);
    }
}
K_THREAD_DEFINE(screen_update_thread, 2048, update_screen_thread_func, NULL, NULL, NULL, 7, 0, 0);

void lvgl_task_handler_thread_func(void *arg1, void *arg2, void *arg3)
{
    while (1)
    {
        lvgl_lock();
        lv_task_handler();
        lvgl_unlock();
        k_msleep(5);
    }
}
K_THREAD_DEFINE(lvgl_task_handler_thread, 4096, lvgl_task_handler_thread_func, NULL, NULL, NULL, 7, 0, 0);