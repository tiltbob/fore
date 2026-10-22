/*
 * Host implementation of the platform hooks: flash is a file-backed array
 * with the same erase/program rules as the STM32 (programming a halfword
 * that is not erased is an error), randomness comes from the OS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/platform.h"
#include "platform_host.h"

static uint8_t mem[STORE_PAGES * FLASH_PAGE_SIZE];
static const char *backing;

static void sync_file(void)
{
    FILE *f;
    if (!backing)
        return;
    f = fopen(backing, "wb");
    if (!f || fwrite(mem, 1, sizeof(mem), f) != sizeof(mem)) {
        perror(backing);
        exit(1);
    }
    fclose(f);
}

void host_flash_open(const char *path)
{
    FILE *f;

    memset(mem, 0xff, sizeof(mem));
    backing = path;
    if (path && (f = fopen(path, "rb"))) {
        if (fread(mem, 1, sizeof(mem), f) != sizeof(mem))
            memset(mem, 0xff, sizeof(mem));
        fclose(f);
    }
}

uint8_t *host_flash_mem(void)
{
    return mem;
}

const uint8_t *flash_page(unsigned page)
{
    return mem + page * FLASH_PAGE_SIZE;
}

int flash_erase(unsigned page)
{
    if (page >= STORE_PAGES)
        return -1;
    memset(mem + page * FLASH_PAGE_SIZE, 0xff, FLASH_PAGE_SIZE);
    sync_file();
    return 0;
}

int flash_program(unsigned page, unsigned off, const void *src, size_t len)
{
    size_t start = (size_t)page * FLASH_PAGE_SIZE + off;
    const uint8_t *s = src;

    if ((off | len) & 1 || start + len > sizeof(mem)) {
        fprintf(stderr, "flash_program: bad range page %u off %u len %zu\n", page, off, len);
        abort();
    }
    for (size_t i = 0; i < len; i += 2) {
        if (mem[start + i] != 0xff || mem[start + i + 1] != 0xff) {
            fprintf(stderr, "flash_program: halfword at %zu not erased\n", start + i);
            abort();
        }
        mem[start + i] = s[i];
        mem[start + i + 1] = s[i + 1];
    }
    sync_file();
    return 0;
}

void rng_fill(uint8_t *buf, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, len, f) != len)
        abort();
    fclose(f);
}

static uint8_t uid[12] = "fore-hostsim";

void host_set_uid(const char *s)
{
    memset(uid, 0, sizeof(uid));
    memcpy(uid, s, strlen(s) < sizeof(uid) ? strlen(s) : sizeof(uid));
}

const uint8_t *device_uid(void)
{
    return uid;
}
