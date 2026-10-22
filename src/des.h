/* Triple DES (EDE, three keys) single-block encryption for the PIV management key. */
#ifndef DES_H
#define DES_H

#include <stdint.h>

void des3_encrypt(uint8_t out[8], const uint8_t in[8], const uint8_t key[24]);

#endif
