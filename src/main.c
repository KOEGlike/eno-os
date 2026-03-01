#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(MAIN, LOG_LEVEL_DBG);

int main(void)
{
        LOG_INF("Hello, World! This is eno-os running on Zephyr RTOS.");

        return 0;
}