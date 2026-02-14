#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/logging/log.h>

#include "sd_card.h"

LOG_MODULE_REGISTER(SD, LOG_LEVEL_DBG);

static int init_sd_card(void)
{
    int ret = 0;
    ret = sd_card_init();
    if (ret != -ENODEV && ret != 0)
    {
        LOG_ERR("Failed to initialize SD card");
        return ret;
    }
    return 0;
}

static int log_sd_card_files_once(void)
{
    int ret = 0;
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
}

void sd_card_thread_func(void *arg1, void *arg2, void *arg3)
{
    if (init_sd_card() == 0)
    {
        while (1)
        {
            log_sd_card_files_once();
            k_msleep(5000);
        }
    }
}

K_THREAD_DEFINE(sd_card_thread_id, 1024, sd_card_thread_func, NULL, NULL, NULL, 7, 0, 0);