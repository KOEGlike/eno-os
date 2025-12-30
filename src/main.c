#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
        while (1)
        {
                printk("lol");
                k_msleep(1000);

        }
        
        return 0;
}
