/*
 * Software model of the STM32F103 USB device peripheral (endpoint registers
 * with their toggle / write-0-to-clear bits, buffer descriptor table, packet
 * memory) plus host-side helpers that play the part of the USB host
 * controller. stm32/usb.c is compiled against it with -DUSB_HW_MOCK.
 */
#ifndef USB_MOCK_H
#define USB_MOCK_H

#include <stddef.h>
#include <stdint.h>

/* Device side, used by stm32/usb.c */
uint16_t mock_rd(unsigned reg);
void mock_wr(unsigned reg, uint16_t v);
uint16_t mock_pma_rd(unsigned off);
void mock_pma_wr(unsigned off, uint16_t v);
void mock_irq_off(void);
void mock_irq_on(void);

#define rd(reg) mock_rd(reg)
#define wr(reg, v) mock_wr((reg), (uint16_t)(v))
#define pma_rd(off) mock_pma_rd(off)
#define pma_wr(off, v) mock_pma_wr((off), (uint16_t)(v))
#define irq_off() mock_irq_off()
#define irq_on() mock_irq_on()

/* Host side: single transactions. Return the byte count (IN), 0 (ACK), or: */
#define MOCK_NAK (-1)
#define MOCK_STALL (-2)
#define MOCK_NORESP (-3)

void mock_bus_reset(void);
int mock_setup(uint8_t addr, const uint8_t setup[8]);
int mock_in(uint8_t addr, unsigned ep, uint8_t *buf, unsigned max, int *toggle);
int mock_out(uint8_t addr, unsigned ep, const uint8_t *data, unsigned len, int toggle);

/* Host side: transfers, like a host controller driver would issue them. */
typedef struct {
    uint8_t addr;
    uint8_t toggle_in[16], toggle_out[16];
} usb_host_dev;

#define HOST_TIMEOUT (-4)
#define HOST_ERROR (-5)

/* Returns bytes transferred in the data stage, or MOCK_STALL / HOST_*. */
int host_control(usb_host_dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                 uint8_t *data, uint16_t length);
int host_bulk_out(usb_host_dev *d, unsigned ep, const uint8_t *data, size_t len, int timeout_ms);
/* Reads until a short packet or max bytes. Returns the count or HOST_*. */
int host_bulk_in(usb_host_dev *d, unsigned ep, uint8_t *buf, size_t max, int timeout_ms);
/* Like the kernel: reset, address, read descriptors, set configuration 1. */
int host_enumerate(usb_host_dev *d, uint8_t addr, uint8_t *dev_desc, uint8_t *cfg_desc, size_t cfg_max);

/* Firmware "main loop" thread: ccid_poll() forever. */
void mock_start_device_thread(void);

#endif
