#ifndef USB_H
#define USB_H

#include <stdint.h>

/* Brings up the USB device peripheral (call with the 48 MHz USB clock running). */
void usb_init(void);
/* USB low-priority interrupt handler. */
void usb_irq(void);

#endif
