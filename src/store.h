/*
 * Persistent storage in the last STORE_PAGES pages of flash:
 *
 *   0, 1    applet state (PINs, management key, private keys), written A/B so
 *           an interrupted update leaves the previous copy intact
 *   2, 3    PIN / PUK failure tallies: one programmed halfword per failure,
 *           so counting an attempt never needs an erase
 *   4..13   data objects (certificates, CHUID, CCC)
 */
#ifndef STORE_H
#define STORE_H

#include <stddef.h>
#include <stdint.h>

#define NUM_KEYS 4

enum { PIN_REF = 0, PUK_REF = 1 };

typedef struct {
    uint8_t present;
    uint8_t pin_policy;
    uint8_t priv[48];
} piv_key;

typedef struct {
    uint8_t pin[8]; /* padded with 0xFF */
    uint8_t puk[8];
    uint8_t mgmt[24];
    uint8_t max_tries[2]; /* indexed by PIN_REF / PUK_REF */
    uint8_t pad[2];
    piv_key keys[NUM_KEYS];
} piv_state;

/* Working copy of the state; call store_save() after changing it. */
extern piv_state g_state;

void store_init(void);
int store_save(void);
/* Erases everything and writes the default state. */
void store_format(void);

int store_failures(int ref);
void store_add_failure(int ref);
void store_clear_failures(int ref);

/* Index into the data object table for a GET/PUT DATA tag, or -1. */
int store_object_index(uint32_t tag);
/* Returns NULL if the object is absent. */
const uint8_t *store_object(int idx, size_t *len);
/* len == 0 deletes. Returns -1 if the object does not fit. */
int store_put_object(int idx, const uint8_t *data, size_t len);

#endif
