#ifndef PLATFORM_HOST_H
#define PLATFORM_HOST_H

#include <stdint.h>

/* Loads flash contents from path (or starts erased); NULL keeps it in memory only. */
void host_flash_open(const char *path);
uint8_t *host_flash_mem(void);
void host_set_uid(const char *s);

#endif
