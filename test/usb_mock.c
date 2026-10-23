#include "usb_mock.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/ccid.h"
#include "../stm32/usb.h"

#define CNTR 0x40
#define ISTR 0x44
#define DADDR 0x4c
#define BTABLE 0x50

#define CTR_RX 0x8000
#define DTOG_RX 0x4000
#define STAT_RX 0x3000
#define SETUP 0x0800
#define EP_TYPE 0x0600
#define EP_KIND 0x0100
#define CTR_TX 0x0080
#define DTOG_TX 0x0040
#define STAT_TX 0x0030
#define EA 0x000f

#define STAT_DISABLED 0
#define STAT_STALL 1
#define STAT_NAK 2
#define STAT_VALID 3

#define FAIL(...)                                  \
    do {                                           \
        fprintf(stderr, "usb_mock: " __VA_ARGS__); \
        fprintf(stderr, "\n");                     \
        abort();                                   \
    } while (0)

static pthread_mutex_t hw = PTHREAD_MUTEX_INITIALIZER;
static uint16_t epr[8], cntr = 0x0003, istr, daddr, btable;
static uint16_t pma[256];

static int ctr_pending(unsigned *ep, int *dir)
{
    for (unsigned i = 0; i < 8; i++) {
        if (epr[i] & (CTR_RX | CTR_TX)) {
            *ep = i;
            *dir = !!(epr[i] & CTR_RX);
            return 1;
        }
    }
    return 0;
}

uint16_t mock_rd(unsigned reg)
{
    unsigned ep;
    int dir;

    if (reg < 0x20 && reg % 4 == 0)
        return epr[reg / 4];
    switch (reg) {
    case CNTR:
        return cntr;
    case ISTR:
        if (ctr_pending(&ep, &dir))
            return (uint16_t)(istr | 0x8000 | (dir ? 0x10 : 0) | ep);
        return istr;
    case DADDR:
        return daddr;
    case BTABLE:
        return btable;
    }
    FAIL("read of unknown register 0x%x", reg);
}

void mock_wr(unsigned reg, uint16_t v)
{
    if (reg < 0x20 && reg % 4 == 0) {
        uint16_t old = epr[reg / 4], nv = old;
        if (!(v & CTR_RX))
            nv &= ~CTR_RX;
        if (!(v & CTR_TX))
            nv &= ~CTR_TX;
        nv ^= v & (DTOG_RX | STAT_RX | DTOG_TX | STAT_TX);
        nv = (nv & ~(EP_TYPE | EP_KIND | EA)) | (v & (EP_TYPE | EP_KIND | EA));
        epr[reg / 4] = nv;
        return;
    }
    switch (reg) {
    case CNTR:
        cntr = v;
        return;
    case ISTR:
        istr &= v & 0x7f00; /* flag bits are write-0-to-clear; CTR/DIR/EP_ID are read-only */
        return;
    case DADDR:
        daddr = v & 0xff;
        return;
    case BTABLE:
        btable = v & 0xfff8;
        return;
    }
    FAIL("write of unknown register 0x%x", reg);
}

uint16_t mock_pma_rd(unsigned off)
{
    if (off & 1 || off >= 512)
        FAIL("PMA read at %u", off);
    return pma[off / 2];
}

void mock_pma_wr(unsigned off, uint16_t v)
{
    if (off & 1 || off >= 512)
        FAIL("PMA write at %u", off);
    pma[off / 2] = v;
}

void mock_irq_off(void)
{
    pthread_mutex_lock(&hw);
}

void mock_irq_on(void)
{
    pthread_mutex_unlock(&hw);
}

static uint16_t bt(unsigned ep, unsigned field)
{
    return pma[(btable + 8 * ep + field) / 2];
}

static void bt_set(unsigned ep, unsigned field, uint16_t v)
{
    pma[(btable + 8 * ep + field) / 2] = v;
}

static unsigned rx_capacity(unsigned ep)
{
    uint16_t c = bt(ep, 6);
    unsigned blocks = (c >> 10) & 0x1f;
    return (c & 0x8000) ? 32 * (blocks + 1) : 2 * blocks;
}

static void set_stat_hw(unsigned ep, uint16_t mask, unsigned shift, unsigned stat)
{
    epr[ep] = (uint16_t)((epr[ep] & ~mask) | (stat << shift));
}

/* The interrupt line: call the handler until nothing unmasked is pending. */
static void run_isr(void)
{
    for (int n = 0; n < 100; n++) {
        unsigned ep;
        int dir;
        int pending = ((cntr & 0x8000) && ctr_pending(&ep, &dir)) || ((cntr & 0x0400) && (istr & 0x0400));
        if (!pending)
            return;
        usb_irq();
    }
    FAIL("interrupt storm");
}

static int addressed(uint8_t addr)
{
    return (daddr & 0x80) && (daddr & 0x7f) == addr && !(cntr & 0x0003);
}

void mock_bus_reset(void)
{
    pthread_mutex_lock(&hw);
    memset(epr, 0, sizeof(epr));
    daddr = 0;
    istr |= 0x0400;
    run_isr();
    pthread_mutex_unlock(&hw);
}

int mock_setup(uint8_t addr, const uint8_t setup[8])
{
    int r = 0;

    pthread_mutex_lock(&hw);
    if (!addressed(addr) || (epr[0] & EP_TYPE) != 0x0200 || ((epr[0] & STAT_RX) >> 12) == STAT_DISABLED) {
        r = MOCK_NORESP;
    } else {
        if (epr[0] & CTR_RX)
            FAIL("SETUP while EP0 still holds an unprocessed packet");
        if (rx_capacity(0) < 8)
            FAIL("EP0 RX buffer too small");
        for (unsigned i = 0; i < 8; i += 2)
            pma[(bt(0, 4) + i) / 2] = (uint16_t)(setup[i] | setup[i + 1] << 8);
        bt_set(0, 6, (uint16_t)((bt(0, 6) & 0xfc00) | 8));
        /* SETUP is always accepted; it forces both data toggles to DATA1. */
        epr[0] |= CTR_RX | SETUP | DTOG_RX | DTOG_TX;
        set_stat_hw(0, STAT_RX, 12, STAT_NAK);
        run_isr();
    }
    pthread_mutex_unlock(&hw);
    return r;
}

int mock_in(uint8_t addr, unsigned ep, uint8_t *buf, unsigned max, int *toggle)
{
    int r;

    pthread_mutex_lock(&hw);
    unsigned stat = (epr[ep] & STAT_TX) >> 4;
    if (!addressed(addr) || stat == STAT_DISABLED || (epr[ep] & EA) != ep) {
        r = MOCK_NORESP;
    } else if (stat == STAT_STALL) {
        r = MOCK_STALL;
    } else if (stat == STAT_NAK) {
        r = MOCK_NAK;
    } else {
        unsigned len = bt(ep, 2) & 0x3ff, a = bt(ep, 0);
        if (epr[ep] & CTR_TX)
            FAIL("IN on EP%u with CTR_TX still set", ep);
        if (len > max)
            FAIL("EP%u sent %u bytes, max packet %u", ep, len, max);
        for (unsigned i = 0; i < len; i++)
            buf[i] = (uint8_t)(pma[(a + (i & ~1u)) / 2] >> (8 * (i & 1)));
        *toggle = !!(epr[ep] & DTOG_TX);
        epr[ep] ^= DTOG_TX;
        epr[ep] |= CTR_TX;
        set_stat_hw(ep, STAT_TX, 4, STAT_NAK);
        r = (int)len;
        run_isr();
    }
    pthread_mutex_unlock(&hw);
    return r;
}

int mock_out(uint8_t addr, unsigned ep, const uint8_t *data, unsigned len, int toggle)
{
    int r = 0;

    pthread_mutex_lock(&hw);
    unsigned stat = (epr[ep] & STAT_RX) >> 12;
    if (!addressed(addr) || stat == STAT_DISABLED || (epr[ep] & EA) != ep) {
        r = MOCK_NORESP;
    } else if (stat == STAT_STALL) {
        r = MOCK_STALL;
    } else if (stat == STAT_NAK) {
        r = MOCK_NAK;
    } else if (toggle != !!(epr[ep] & DTOG_RX)) {
        /* Retransmission of a packet we already have: ACK and drop. */
    } else {
        unsigned a = bt(ep, 4);
        if (epr[ep] & CTR_RX)
            FAIL("OUT on EP%u with CTR_RX still set", ep);
        if (len > rx_capacity(ep))
            FAIL("EP%u RX overflow: %u > %u", ep, len, rx_capacity(ep));
        for (unsigned i = 0; i < len; i += 2)
            pma[(a + i) / 2] = (uint16_t)(data[i] | (i + 1 < len ? data[i + 1] << 8 : 0));
        bt_set(ep, 6, (uint16_t)((bt(ep, 6) & 0xfc00) | len));
        epr[ep] ^= DTOG_RX;
        epr[ep] &= ~SETUP;
        epr[ep] |= CTR_RX;
        set_stat_hw(ep, STAT_RX, 12, STAT_NAK);
        run_isr();
    }
    pthread_mutex_unlock(&hw);
    return r;
}

/* ---- host controller side ---- */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static void backoff(void)
{
    usleep(50);
}

int host_control(usb_host_dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                 uint8_t *data, uint16_t length)
{
    uint8_t s[8] = {type, req, (uint8_t)value, (uint8_t)(value >> 8),
                    (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)length, (uint8_t)(length >> 8)};
    int r, toggle, expect = 1, total = 0;
    long deadline = now_ms() + 5000;

    if (mock_setup(d->addr, s) != 0)
        return HOST_ERROR;
    if (type & 0x80) {
        while (total < length) {
            uint8_t pkt[64];
            r = mock_in(d->addr, 0, pkt, 64, &toggle);
            if (r == MOCK_NAK) {
                if (now_ms() > deadline)
                    return HOST_TIMEOUT;
                backoff();
                continue;
            }
            if (r < 0)
                return r;
            if (toggle != expect) {
                fprintf(stderr, "host: EP0 IN data toggle %d, expected %d\n", toggle, expect);
                return HOST_ERROR;
            }
            expect ^= 1;
            if (r > length - total) {
                fprintf(stderr, "host: EP0 babble\n");
                return HOST_ERROR;
            }
            memcpy(data + total, pkt, (size_t)r);
            total += r;
            if (r < 64)
                break;
        }
        while ((r = mock_out(d->addr, 0, NULL, 0, 1)) == MOCK_NAK) {
            if (now_ms() > deadline)
                return HOST_TIMEOUT;
            backoff();
        }
        return r < 0 ? r : total;
    }
    if (length)
        return HOST_ERROR; /* no OUT data stages in this device */
    for (;;) {
        uint8_t pkt[64];
        r = mock_in(d->addr, 0, pkt, 64, &toggle);
        if (r == MOCK_NAK) {
            if (now_ms() > deadline)
                return HOST_TIMEOUT;
            backoff();
            continue;
        }
        if (r < 0)
            return r;
        if (r != 0 || toggle != 1) {
            fprintf(stderr, "host: bad status stage (len %d, toggle %d)\n", r, toggle);
            return HOST_ERROR;
        }
        return 0;
    }
}

int host_bulk_out(usb_host_dev *d, unsigned ep, const uint8_t *data, size_t len, int timeout_ms)
{
    long deadline = now_ms() + (timeout_ms ? timeout_ms : 1000000);
    size_t off = 0;

    do {
        unsigned n = len - off > 64 ? 64 : (unsigned)(len - off);
        int r = mock_out(d->addr, ep, data + off, n, d->toggle_out[ep]);
        if (r == MOCK_NAK) {
            if (now_ms() > deadline)
                return HOST_TIMEOUT;
            backoff();
            continue;
        }
        if (r < 0)
            return r;
        d->toggle_out[ep] ^= 1;
        off += n;
    } while (off < len);
    return (int)off;
}

int host_bulk_in(usb_host_dev *d, unsigned ep, uint8_t *buf, size_t max, int timeout_ms)
{
    long deadline = now_ms() + (timeout_ms ? timeout_ms : 1000000);
    size_t total = 0;

    while (total < max) {
        uint8_t pkt[64];
        int toggle, r = mock_in(d->addr, ep, pkt, 64, &toggle);
        if (r == MOCK_NAK) {
            if (now_ms() > deadline)
                return HOST_TIMEOUT;
            backoff();
            continue;
        }
        if (r < 0)
            return r;
        if (toggle != d->toggle_in[ep]) {
            fprintf(stderr, "host: EP%u IN data toggle %d, expected %d\n", ep, toggle, d->toggle_in[ep]);
            return HOST_ERROR;
        }
        d->toggle_in[ep] ^= 1;
        if ((size_t)r > max - total) {
            fprintf(stderr, "host: EP%u babble\n", ep);
            return HOST_ERROR;
        }
        memcpy(buf + total, pkt, (size_t)r);
        total += (size_t)r;
        if (r < 64)
            break;
    }
    return (int)total;
}

int host_enumerate(usb_host_dev *d, uint8_t addr, uint8_t *dev_desc, uint8_t *cfg_desc, size_t cfg_max)
{
    int r;

    memset(d, 0, sizeof(*d));
    mock_bus_reset();
    /* Linux asks for 64 bytes of the device descriptor first, then resets again. */
    if ((r = host_control(d, 0x80, 6, 0x0100, 0, dev_desc, 64)) != 18)
        return r < 0 ? r : HOST_ERROR;
    mock_bus_reset();
    if ((r = host_control(d, 0x00, 5, addr, 0, NULL, 0)) != 0)
        return r;
    d->addr = addr;
    if ((r = host_control(d, 0x80, 6, 0x0100, 0, dev_desc, 18)) != 18)
        return r < 0 ? r : HOST_ERROR;
    if ((r = host_control(d, 0x80, 6, 0x0200, 0, cfg_desc, 9)) != 9)
        return r < 0 ? r : HOST_ERROR;
    unsigned total = cfg_desc[2] | cfg_desc[3] << 8;
    if (total > cfg_max)
        return HOST_ERROR;
    if ((r = host_control(d, 0x80, 6, 0x0200, 0, cfg_desc, (uint16_t)total)) != (int)total)
        return r < 0 ? r : HOST_ERROR;
    if ((r = host_control(d, 0x00, 9, 1, 0, NULL, 0)) != 0)
        return r;
    memset(d->toggle_in, 0, sizeof(d->toggle_in));
    memset(d->toggle_out, 0, sizeof(d->toggle_out));
    return (int)total;
}

static void *device_thread(void *arg)
{
    (void)arg;
    for (;;) {
        ccid_poll();
        usleep(20);
    }
    return NULL;
}

void mock_start_device_thread(void)
{
    pthread_t t;
    usb_init();
    pthread_create(&t, NULL, device_thread, NULL);
    pthread_detach(t);
}

void platform_activity(int on)
{
    (void)on;
}
