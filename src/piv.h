/* Minimal PIV applet (SP 800-73-4 subset plus the Yubico key import extensions). */
#ifndef PIV_H
#define PIV_H

#include <stddef.h>
#include <stdint.h>

#define PIV_VERSION_MAJOR 0
#define PIV_VERSION_MINOR 1
#define PIV_VERSION_PATCH 0

/* Largest response piv_apdu() writes: 256 data bytes + SW1 SW2. */
#define PIV_MAX_RESPONSE 258

/* Answer-to-reset: T=1, historical bytes "fore-PIV" as card issuer's data. */
extern const uint8_t piv_atr[];
extern const size_t piv_atr_len;

/* Loads persistent state. Call once at startup. */
void piv_init(void);
/* Card reset (power on/off): drops PIN and management key authentication. */
void piv_reset(void);
/* Processes one short command APDU, writes the response APDU and returns its length. */
size_t piv_apdu(const uint8_t *apdu, size_t len, uint8_t *resp);
/* True if the command may take long enough to warrant a CCID time extension. */
int piv_is_slow(const uint8_t *apdu, size_t len);
/* 31-bit serial number derived from the device UID. */
uint32_t piv_serial(void);

#endif
