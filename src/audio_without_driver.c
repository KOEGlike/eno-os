#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/logging/log.h>

#include <zephyr/audio/codec.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/i2c.h>

#include "audio_snippet.h"
#include "audio_without_driver.h"

LOG_MODULE_REGISTER(AUDIO, LOG_LEVEL_DBG);

#define DAC_NODE DT_NODELABEL(tad5212)
static const struct i2c_dt_spec dac = I2C_DT_SPEC_GET(DAC_NODE);

#define SAMPLE_FREQUENCY 16000
#define SAMPLE_BIT_WIDTH 16
#define BYTES_PER_SAMPLE 2
#define NUMBER_OF_CHANNELS (2U) /* Such block length provides an echo with the delay of 100 ms. */
#define SAMPLES_PER_BLOCK ((SAMPLE_FREQUENCY / 10) * NUMBER_OF_CHANNELS)
#define INITIAL_BLOCKS 4
#define TIMEOUT (2000U)

#define BLOCK_SIZE (BYTES_PER_SAMPLE * SAMPLES_PER_BLOCK)
#define BLOCK_COUNT (INITIAL_BLOCKS + 32)

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(mem_slab, __nocache, BLOCK_SIZE, BLOCK_COUNT, 4);

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

#define CFG_META_SWITCH (255)
#define CFG_META_DELAY (254)
#define CFG_META_BURST (253)

// Example implementation.  Call like:
//     transmit_registers(registers, sizeof(registers)/sizeof(registers[0]));
void transmit_registers(cfg_reg *r, int n)
{
    int i = 0;
    while (i < n)
    {
        switch (r[i].command)
        {
        case CFG_META_SWITCH:
            // Used in legacy applications.  Ignored here.
            break;
        case CFG_META_DELAY:
            k_msleep(r[i].param);
            break;
        case CFG_META_BURST:
            i2c_write_dt(&dac, (unsigned char *)&r[i + 1], r[i].param);
            i += (r[i].param / 2) + 1;
            break;
        default:
            i2c_write_dt(&dac, (unsigned char *)&r[i], 2);
            break;
        }
        i++;
    }
}

int configure_audio()
{
    int ret = 0;

    if (!device_is_ready(i2s_dev))
    {
        LOG_ERR("%s is not ready\n", i2s_dev->name);
        return -1;
    }

    if (!i2c_is_ready_dt(&dac))
    {
        LOG_ERR("%s is not ready", dac.bus->name);
        return -1;
    }

    transmit_registers(registers, sizeof(registers) / sizeof(registers[0]));

    k_msleep(1000);

    ret = i2s_configure(i2s_dev, I2S_DIR_TX, &audio_bus_config);
    if (ret < 0)
    {
        LOG_ERR("i2s_configure failed: %d", ret);
        return -1;
    }

    return 0;
}

int play_sine_wave()
{
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

void audio_thread_func(void *arg1, void *arg2, void *arg3)
{
    if (configure_audio() == 0)
    {
        play_sine_wave();
    }
}

K_THREAD_DEFINE(audio_thread_id, 4096, audio_thread_func, NULL, NULL, NULL, 7, 0, 0);
