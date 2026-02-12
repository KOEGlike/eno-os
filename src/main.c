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

#include <zephyr/audio/codec.h>
#include <zephyr/drivers/i2s.h>

#include "sd_card.h"
#include "audio_snippet.h"

#include <lvgl.h>
#include <lvgl_zephyr.h>

LOG_MODULE_REGISTER(MAIN, LOG_LEVEL_DBG);

#define SAMPLE_FREQUENCY 16000
#define SAMPLE_BIT_WIDTH 16
#define BYTES_PER_SAMPLE 2
#define NUMBER_OF_CHANNELS (2U) /* Such block length provides an echo with the delay of 100 ms. */
#define SAMPLES_PER_BLOCK ((SAMPLE_FREQUENCY / 10) * NUMBER_OF_CHANNELS)
#define INITIAL_BLOCKS 4
#define TIMEOUT (2000U)

#define BLOCK_SIZE (BYTES_PER_SAMPLE * SAMPLES_PER_BLOCK)
#define BLOCK_COUNT (INITIAL_BLOCKS + 32)
// Motor
#define MOTOR_NODE DT_NODELABEL(motor)
static const struct gpio_dt_spec motor = GPIO_DT_SPEC_GET(MOTOR_NODE, motor_gpios);

// AS5600
#define AS5600_NODE DT_NODELABEL(as5600)
static const struct device *as5600 = DEVICE_DT_GET(AS5600_NODE);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
static const struct device *display = DEVICE_DT_GET(DISPLAY_NODE);

// PMIC
#define NPM13XX_DEVICE(dev) DEVICE_DT_GET(DT_NODELABEL(npm1300_##dev))
static const struct device *pmic = NPM13XX_DEVICE(pmic);
static const struct device *leds = NPM13XX_DEVICE(leds);
static const struct device *regulators = NPM13XX_DEVICE(regulators);
static const struct device *ldsw = NPM13XX_DEVICE(hall_pwr);

// DAC
#define DAC_NODE DT_NODELABEL(tad5212)
static const struct device *dac = DEVICE_DT_GET(DAC_NODE);

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(mem_slab, __nocache, BLOCK_SIZE, BLOCK_COUNT, 4);

struct audio_codec_cfg dac_cfg = {
    .dai_route = AUDIO_ROUTE_PLAYBACK,
    .dai_type = AUDIO_DAI_TYPE_I2S,
    .dai_cfg.i2s.word_size = SAMPLE_BIT_WIDTH,
    .dai_cfg.i2s.channels = 2,
    .dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_I2S,
    .dai_cfg.i2s.options = I2S_OPT_FRAME_CLK_SLAVE | I2S_OPT_BIT_CLK_SLAVE,
    .dai_cfg.i2s.frame_clk_freq = SAMPLE_FREQUENCY,
    .dai_cfg.i2s.mem_slab = &mem_slab,
    .dai_cfg.i2s.block_size = BLOCK_SIZE,
};

// I2S
#define I2S_NODE DT_NODELABEL(i2s0)
static const struct device *i2s_dev = DEVICE_DT_GET(I2S_NODE);

struct i2s_config audio_bus_config = {
    .word_size = SAMPLE_BIT_WIDTH,
    .channels = NUMBER_OF_CHANNELS,
    .format = I2S_FMT_DATA_FORMAT_I2S,
    .options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
    .frame_clk_freq = SAMPLE_FREQUENCY,
    .mem_slab = &mem_slab,
    .block_size = BLOCK_SIZE,
    .timeout = TIMEOUT,
};

void turn_on_one_led(uint8_t led)
{
        for (uint32_t i = 0; i <= 2; i++)
        {
                led_off(leds, i);
        }
        led_on(leds, led);
}

static bool trigger_command(const struct device *i2s_dev_codec, enum i2s_trigger_cmd cmd)
{
        int ret;

        ret = i2s_trigger(i2s_dev_codec, I2S_DIR_TX, cmd);
        if (ret < 0)
        {
                printk("Failed to trigger command %d on TX: %d\n", cmd, ret);
                return false;
        }

        return true;
}

int init_display_device(void);
int init_lvgl(void);
void setup_lvgl_hello_screen(void);
int configure_audio(void);

int main(void)
{
        printk("APP: main() entered\n");

        LOG_INF("Booted up");

        int ret = 0;

        // ret = init_motor_device();
        // ret = init_as5600_device();
        ret = init_display_device();
        ret = configure_audio();

        if (!device_is_ready(leds))
        {
                LOG_ERR("Error: led device is not ready\n");
                return -1;
        }
        turn_on_one_led(1);

        // ret = init_sd_card();
        ret = init_lvgl();
        k_msleep(2000);
        setup_lvgl_hello_screen();
        // ret = log_sd_card_files_once();

        LOG_INF("Starting audio playback");

        turn_on_one_led(0);

        // Do forever
        while (1)
        {

                LOG_INF("WOKE UP ON");

                // poll_as5600_angle();
                // ret = log_sd_card_files_once();

                k_msleep(3000);
        }
}

int configure_audio() {
        int ret = 0;
        
        if (!device_is_ready(i2s_dev))
        {
                LOG_ERR("%s is not ready\n", i2s_dev->name);
                return -1;
        }

        if (!device_is_ready(dac))
        {
                LOG_ERR("%s is not ready", dac->name);
                return -1;
        }

        ret = audio_codec_configure(dac, &dac_cfg);
        if (ret < 0)
        {
                LOG_ERR("audio_codec_configure failed: %d", ret);
                return -1;
        }
        k_msleep(1000);

        ret = i2s_configure(i2s_dev, I2S_DIR_TX, &audio_bus_config);
        if (ret < 0)
        {
                LOG_ERR("i2s_configure failed: %d", ret);
                return -1;
        }

        audio_codec_set_property(dac, AUDIO_PROPERTY_OUTPUT_VOLUME,
                                 AUDIO_CHANNEL_ALL, (audio_property_value_t){.vol = 150}); // Volume: 0-255

        audio_codec_start_output(dac);

        return 0;

}

int play_sine_wave() {
        int ret = 0;
        for (;;)
        {
                bool started = false;

                while (1)
                {
                        void *mem_block;
                        uint32_t block_size = BLOCK_SIZE;
                        int i;

                        for (i = 0; i < INITIAL_BLOCKS; i++)
                        {

                                BUILD_ASSERT(
                                    BLOCK_SIZE <= __16kHz16bit_stereo_sine_pcm_len,
                                    "BLOCK_SIZE is bigger than test sine wave buffer size.");
                                mem_block = (void *)&__16kHz16bit_stereo_sine_pcm;

                                ret = i2s_buf_write(i2s_dev, mem_block, block_size);
                                if (ret < 0)
                                {
                                        LOG_ERR("Failed to write data: %d", ret);
                                        break;
                                }
                                LOG_INF("Wrote block %d", i);
                        }
                        if (ret < 0)
                        {
                                LOG_ERR("error %d", ret);
                                break;
                        }
                        if (!started)
                        {
                                i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
                                started = true;
                        }
                }
                if (!trigger_command(i2s_dev, I2S_TRIGGER_DROP))
                {
                        LOG_ERR("Send I2S trigger DRAIN failed: %d", ret);
                        return 0;
                }

                LOG_INF("Streams stopped");
                return 0;
        }
}

static int init_motor_device(void)
{
#if 0
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
#endif
        return 0;
}

static int init_as5600_device(void)
{
#if 0
        if (!device_is_ready(as5600))
        {
                LOG_ERR("AS5600 device not ready");
                return 0;
        }
#endif
        return 0;
}

int init_display_device(void)
{

        if (!device_is_ready(display))
        {
                LOG_ERR("display not ready: %s", display ? display->name : "(null)");
                LOG_PANIC();
                return -1;
        }
        return 0;
}

static int init_sd_card(void)
{
#if 0
        ret = sd_card_init();
        if (ret != -ENODEV && ret != 0)
        {
                LOG_ERR("Failed to initialize SD card");
                return ret;
        }
#endif
        return 0;
}

int init_lvgl(void)
{

        lv_init();

        LOG_INF("INITIALIZED LVGL");

        if (display_blanking_off(display))
        {
                LOG_INF("Failed to turn off display blanking!");
                return 0;
        }
        LOG_INF("Display blanking is off. Screen should be cleared by full refresh.");
        return 0;
}


#define SCREEN_UPDATE_STACK_SIZE    2048
K_THREAD_STACK_DEFINE(screen_update_stack, SCREEN_UPDATE_STACK_SIZE);
static struct k_thread screen_update_thread_data;
void update_screen_thread_func(void *arg1, void *arg2, void *arg3)
{
        lv_obj_t *label = (lv_obj_t *)arg1;
        int counter = 0;
        char buf[64];
        while (1)
        {
                snprintf(buf, sizeof(buf), "Hellooo, World!\n Counter: %d", counter++);
                lvgl_lock();
                lv_label_set_text(label, buf);
                lvgl_unlock();
                LOG_INF("Updated label text: %s", buf);
                k_msleep(2000);
        }
}

#define LVGL_TASK_HANDLER_STACK_SIZE 4096
K_THREAD_STACK_DEFINE(lvgl_task_handler_stack, LVGL_TASK_HANDLER_STACK_SIZE);
static struct k_thread lvgl_task_handler_thread_data;
void lvgl_task_handler_thread_func(void *arg1, void *arg2, void *arg3)
{
        uint32_t log_counter = 0;
        while (1)
        {
                lvgl_lock();
                lv_task_handler();
                lvgl_unlock();
                if ((log_counter++ % 200U) == 0U)
                {
                        LOG_INF("Called lv_task_handler");
                }
                k_msleep(5);
        }
}

void setup_lvgl_hello_screen(void)
{
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

        // Create a thread to update the label text every 2 seconds
        k_thread_create(&screen_update_thread_data, screen_update_stack, SCREEN_UPDATE_STACK_SIZE,
                        update_screen_thread_func, label, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
        // Create a thread to call lv_task_handler periodically
        k_thread_create(&lvgl_task_handler_thread_data, lvgl_task_handler_stack, LVGL_TASK_HANDLER_STACK_SIZE,
                        lvgl_task_handler_thread_func, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
}

static int log_sd_card_files_once(void)
{
#if 0
        char buf[512];
        memset(buf, 0, sizeof(buf)); // <-- ensure empty string if nothing is written
        size_t buf_size = sizeof(buf);

        ret = sd_card_list_files(NULL, buf, &buf_size, true);
        if (ret)
        {
                LOG_ERR("sd_card_list_files failed: %d", ret);
                return ret;
        }

        buf[sizeof(buf) - 1] = '\0'; // <-- hard stop in case callee forgot
        LOG_INF("SD list (%u bytes cap, size now %u):\n%s",
                (unsigned)sizeof(buf), (unsigned)buf_size, buf);
#endif
        return 0;
}

static void poll_as5600_angle(void)
{
#if 0
        int ret = sensor_sample_fetch(as5600);
        if (ret)
        {
                LOG_ERR("sensor_sample_fetch failed: %d", ret);
                k_msleep(1);
                return;
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
#endif
}