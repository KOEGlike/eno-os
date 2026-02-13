#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/sensor.h>

LOG_MODULE_REGISTER(ENCODER, LOG_LEVEL_DBG);

// AS5600
#define AS5600_NODE DT_NODELABEL(as5600)
static const struct device *as5600 = DEVICE_DT_GET(AS5600_NODE);

void log_encoder_thread_func(void *arg1, void *arg2, void *arg3)
{
    if (!device_is_ready(as5600))
    {
        LOG_ERR("AS5600 device not ready");
        return;
    }

    while (1)
    {
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
        k_msleep(100);
    }
}

K_THREAD_DEFINE(log_encoder_thread_id, 1024, log_encoder_thread_func, NULL, NULL, NULL, 7, 0, 0);