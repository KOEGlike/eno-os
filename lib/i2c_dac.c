#include "i2c_dac_adresses.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2c.h>

LOG_MODULE_REGISTER(I2C_DAC, LOG_LEVEL_DBG);

// Example implementation.  Call like:
//     transmit_registers(registers, sizeof(registers)/sizeof(registers[0]));
void transmit_registers(cfg_reg *r, int n, const struct device *dac)
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
            i2c_write_dt(dac, (unsigned char *)&r[i + 1], r[i].param);
            i += (r[i].param / 2) + 1;
            break;
        default:
            i2c_write_dt(dac, (unsigned char *)&r[i], 2);
            break;
        }
        i++;
    }
}

int initialize_dac(const struct device *dac)
{
    if (!i2c_is_ready_dt(dac))
    {
        LOG_ERR("%s is not ready", dac->name);
        return -1;
    }

    transmit_registers(initialization_registers, sizeof(initialization_registers) / sizeof(initialization_registers[0]), dac);

    return 0;
}

int power_on_dac(const struct device *dac)
{
    if (!i2c_is_ready_dt(dac))
    {
        LOG_ERR("%s is not ready", dac->name);
        return -1;
    }

    transmit_registers(power_on_registers, sizeof(power_on_registers) / sizeof(power_on_registers[0]), dac);

    return 0;
}

int power_off_dac(const struct device *dac)
{
    if (!i2c_is_ready_dt(dac))
    {
        LOG_ERR("%s is not ready", dac->name);
        return -1;
    }

    transmit_registers(power_off_registers, sizeof(power_off_registers) / sizeof(power_off_registers[0]), dac);

    return 0;
}