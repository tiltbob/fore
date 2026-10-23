/* Vector table and C runtime start-up for the STM32F103. */
#include <stdint.h>
#include <string.h>

#include "stm32f103.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

int main(void);
void usb_irq(void);
void systick_handler(void);

void reset_handler(void)
{
    memcpy(&_sdata, &_sidata, (size_t)((uint8_t *)&_edata - (uint8_t *)&_sdata));
    memset(&_sbss, 0, (size_t)((uint8_t *)&_ebss - (uint8_t *)&_sbss));
    main();
    for (;;)
        ;
}

/* Anything unexpected: start over. */
static void fault_handler(void)
{
    SCB_AIRCR = 0x05fa0004; /* VECTKEY | SYSRESETREQ */
    for (;;)
        ;
}

__attribute__((section(".isr_vector"), used))
static void (*const vectors[16 + 43])(void) = {
    [0] = (void (*)(void))&_estack,
    [1] = reset_handler,
    [2] = fault_handler,  /* NMI */
    [3] = fault_handler,  /* HardFault */
    [4] = fault_handler,  /* MemManage */
    [5] = fault_handler,  /* BusFault */
    [6] = fault_handler,  /* UsageFault */
    [11] = fault_handler, /* SVCall */
    [12] = fault_handler, /* DebugMon */
    [14] = fault_handler, /* PendSV */
    [15] = systick_handler,
    [16 + USB_LP_IRQ] = usb_irq,
};
