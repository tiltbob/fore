/*
 * Hooks the portable code (PIV applet, storage) needs from the platform.
 * Implemented in stm32/ for the device and in host/ for the simulator.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#define FLASH_PAGE_SIZE 1024u
#define STORE_PAGES 14u

/* Memory-mapped view of storage page `page` (0 <= page < STORE_PAGES). */
const uint8_t *flash_page(unsigned page);
/* Erases a page to 0xFF. Returns 0 on success. */
int flash_erase(unsigned page);
/* Programs bytes into erased flash at byte offset off from the start of page
 * (the write may run on into the following pages). off and len must be even.
 * Returns 0 on success. */
int flash_program(unsigned page, unsigned off, const void *src, size_t len);

/* Fills buf with unpredictable bytes. */
void rng_fill(uint8_t *buf, size_t len);

/* 12 bytes that identify this device (the MCU unique ID on hardware). */
const uint8_t *device_uid(void);

#endif
