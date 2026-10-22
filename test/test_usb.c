/*
 * Drives stm32/usb.c + src/ccid.c + the applet through the peripheral model
 * the way a USB host would: enumeration, descriptor checks, CCID framing
 * (multi-packet messages, ZLPs, time extensions, errors), halts and resets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host/platform_host.h"
#include "../src/des.h"
#include "../src/piv.h"
#include "usb_mock.h"

static int failures;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            failures++;                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
        }                                                        \
    } while (0)

static usb_host_dev dev;
static uint8_t seq;

/* Sends a CCID message and returns the reply length (excluding time extensions). */
static int ccid(uint8_t type, uint8_t slot, const uint8_t *data, size_t len, uint8_t *resp, size_t max,
                int *extensions)
{
    uint8_t msg[400];
    int r;

    msg[0] = type;
    msg[1] = (uint8_t)len;
    msg[2] = (uint8_t)(len >> 8);
    msg[3] = msg[4] = 0;
    msg[5] = slot;
    msg[6] = ++seq;
    msg[7] = msg[8] = msg[9] = 0;
    if (len)
        memcpy(msg + 10, data, len);
    if ((r = host_bulk_out(&dev, 1, msg, 10 + len, 2000)) != (int)(10 + len))
        return -100 + r;
    if (extensions)
        *extensions = 0;
    for (;;) {
        r = host_bulk_in(&dev, 1, resp, max, 5000);
        if (r < 10)
            return r < 0 ? r : -99;
        if (resp[6] != seq) {
            fprintf(stderr, "bSeq mismatch %u != %u\n", resp[6], seq);
            return -98;
        }
        if (r != 10 + (resp[1] | resp[2] << 8))
            return -97;
        if ((resp[7] & 0xc0) == 0x80) {
            if (extensions)
                (*extensions)++;
            continue;
        }
        return r;
    }
}

static int xfr(const uint8_t *apdu, size_t len, uint8_t *resp, int *ext)
{
    static uint8_t buf[400];
    int r = ccid(0x6f, 0, apdu, len, buf, sizeof(buf), ext);
    if (r < 10 || buf[0] != 0x80 || buf[7] != 0)
        return -1;
    memcpy(resp, buf + 10, (size_t)r - 10);
    return r - 10;
}

static unsigned sw(const uint8_t *r, int n)
{
    return n >= 2 ? (unsigned)(r[n - 2] << 8 | r[n - 1]) : 0;
}

static void test_descriptors(const uint8_t *d, const uint8_t *c, int clen)
{
    CHECK(d[0] == 18 && d[1] == 1 && d[4] == 0 && d[7] == 64, "device descriptor header");
    CHECK((d[8] | d[9] << 8) == 0x15e1 && (d[10] | d[11] << 8) == 0x2007, "VID/PID");
    CHECK(clen == (c[2] | c[3] << 8) && c[4] == 1, "config length/interfaces");

    int pos = 9, ifaces = 0, eps = 0, ccid_desc = 0;
    while (pos + 2 <= clen) {
        const uint8_t *p = c + pos;
        CHECK(p[0] >= 2 && pos + p[0] <= clen, "descriptor overruns config at %d", pos);
        if (p[0] < 2)
            break;
        if (p[1] == 4) {
            ifaces++;
            CHECK(p[5] == 0x0b && p[4] == 3, "CCID interface with 3 endpoints");
        } else if (p[1] == 0x21) {
            ccid_desc++;
            CHECK(p[0] == 54, "CCID class descriptor is 54 bytes");
            unsigned feat = p[40] | p[41] << 8 | p[42] << 16 | (unsigned)p[43] << 24;
            unsigned maxmsg = p[44] | p[45] << 8 | p[46] << 16 | (unsigned)p[47] << 24;
            CHECK((feat & 0x70000) == 0x20000, "short APDU level exchange");
            CHECK(maxmsg >= 271, "dwMaxCCIDMessageLength %u", maxmsg);
            CHECK(c[pos - 9 + 1] == 4, "class descriptor directly after the interface");
        } else if (p[1] == 5) {
            eps++;
        }
        pos += p[0];
    }
    CHECK(ifaces == 1 && ccid_desc == 1 && eps == 3 && pos == clen, "config structure");
}

static void test_control(void)
{
    uint8_t buf[256];
    int r;

    r = host_control(&dev, 0x80, 6, 0x0300, 0, buf, 255);
    CHECK(r == 4 && buf[2] == 0x09 && buf[3] == 0x04, "language IDs");
    r = host_control(&dev, 0x80, 6, 0x0302, 0x0409, buf, 255);
    CHECK(r == 18 && buf[1] == 3 && memcmp(buf + 2, "f\0o\0r\0e\0 \0P\0I\0V\0", 16) == 0, "product string");
    r = host_control(&dev, 0x80, 6, 0x0303, 0x0409, buf, 2);
    CHECK(r == 2 && buf[0] == 18, "serial string, length probe");
    r = host_control(&dev, 0x80, 6, 0x0303, 0x0409, buf, 255);
    CHECK(r == 18, "serial string");
    r = host_control(&dev, 0x80, 6, 0x0600, 0, buf, 10);
    CHECK(r == MOCK_STALL, "device qualifier must stall (full speed only)");
    r = host_control(&dev, 0x80, 0, 0, 0, buf, 2);
    CHECK(r == 2 && buf[0] == 0 && buf[1] == 0, "GET_STATUS after a stall");
    r = host_control(&dev, 0x80, 8, 0, 0, buf, 1);
    CHECK(r == 1 && buf[0] == 1, "GET_CONFIGURATION");
    r = host_control(&dev, 0xc0, 0x55, 0, 0, buf, 4);
    CHECK(r == MOCK_STALL, "vendor request stalls");
    r = host_control(&dev, 0x21, 1, 0x0100, 0, NULL, 0);
    CHECK(r == 0, "CCID ABORT accepted");
    /* The full configuration descriptor is 93 bytes: two packets. */
    r = host_control(&dev, 0x80, 6, 0x0200, 0, buf, 255);
    CHECK(r == 93, "config descriptor in two packets (%d)", r);
    /* Exactly 64 bytes requested: no ZLP expected, host stops at wLength. */
    r = host_control(&dev, 0x80, 6, 0x0200, 0, buf, 64);
    CHECK(r == 64, "config descriptor truncated to 64");
}

static void test_interrupt(void)
{
    uint8_t buf[8];
    int toggle, r = mock_in(dev.addr, 2, buf, 8, &toggle);
    CHECK(r == 2 && buf[0] == 0x50 && buf[1] == 0x03 && toggle == 0, "NotifySlotChange");
    r = mock_in(dev.addr, 2, buf, 8, &toggle);
    CHECK(r == MOCK_NAK, "interrupt endpoint idle afterwards");
}

static void test_ccid_basics(void)
{
    uint8_t resp[400], apdu[300];
    int r, ext;

    r = ccid(0x65, 0, NULL, 0, resp, sizeof(resp), NULL);
    CHECK(r == 10 && resp[0] == 0x81 && resp[7] == 0x01, "slot status before power on");
    r = ccid(0x6f, 0, (const uint8_t *)"\x00\xa4\x04\x00", 4, resp, sizeof(resp), NULL);
    CHECK(r == 10 && resp[7] == 0x41, "XfrBlock to an unpowered card fails");
    r = ccid(0x62, 0, NULL, 0, resp, sizeof(resp), NULL);
    CHECK(r == 10 + (int)piv_atr_len && resp[0] == 0x80 && resp[7] == 0 &&
              memcmp(resp + 10, piv_atr, piv_atr_len) == 0, "power on returns the ATR");
    r = ccid(0x65, 0, NULL, 0, resp, sizeof(resp), NULL);
    CHECK(r == 10 && resp[7] == 0x00, "slot status active");
    r = ccid(0x65, 1, NULL, 0, resp, sizeof(resp), NULL);
    CHECK(r == 10 && resp[7] == 0x42 && resp[8] == 5, "bad slot");
    r = ccid(0x6c, 0, NULL, 0, resp, sizeof(resp), NULL);
    CHECK(r == 17 && resp[0] == 0x82 && resp[9] == 1, "GetParameters");
    r = ccid(0x6b, 0, (const uint8_t *)"x", 1, resp, sizeof(resp), NULL);
    CHECK(r == 10 && resp[0] == 0x83 && resp[7] == 0x40, "Escape unsupported");
    r = ccid(0x71, 0, NULL, 0, resp, sizeof(resp), NULL);
    CHECK(r == 10 && resp[0] == 0x81 && resp[7] == 0x40 && resp[8] == 0, "unknown command");

    static const uint8_t select[] = {0x00, 0xa4, 0x04, 0x00, 0x05, 0xa0, 0x00, 0x00, 0x03, 0x08, 0x00};
    r = xfr(select, sizeof(select), resp, &ext);
    CHECK(r == 42 && resp[0] == 0x61 && sw(resp, r) == 0x9000 && ext == 0, "SELECT over CCID");

    /* 54-byte APDU: a 64-byte CCID message, no ZLP from the host. */
    memset(apdu, 0, sizeof(apdu));
    memcpy(apdu, "\x00\x20\x00\x80\x31", 5);
    r = xfr(apdu, 54, resp, NULL);
    CHECK(r == 2 && sw(resp, r) == 0x6a80, "64-byte message (VERIFY with bad length)");

    /* Oversized message (extended APDU): rejected, and framing survives. */
    memset(apdu, 0xaa, sizeof(apdu));
    r = ccid(0x6f, 0, apdu, 300, resp, sizeof(resp), NULL);
    CHECK(r == 10 && resp[7] == 0x40 && resp[8] == 1, "oversized message rejected");
    r = xfr(select, sizeof(select), resp, NULL);
    CHECK(r == 42, "still in sync after an oversized message");
}

/* External authenticate with the default management key (only needs encryption). */
static int mgmt_auth(void)
{
    static const uint8_t key[24] = {1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t resp[300], cmd[17] = {0x00, 0x87, 0x03, 0x9b, 0x04, 0x7c, 0x02, 0x81, 0x00};
    int r = xfr(cmd, 9, resp, NULL);

    if (r != 14 || resp[2] != 0x81 || sw(resp, r) != 0x9000)
        return -1;
    memcpy(cmd, "\x00\x87\x03\x9b\x0c\x7c\x0a\x82\x08", 9);
    des3_encrypt(cmd + 9, resp + 4, key);
    r = xfr(cmd, 17, resp, NULL);
    return sw(resp, r) == 0x9000 ? 0 : -1;
}

static void test_ccid_long(void)
{
    uint8_t resp[400], cmd[300];
    int r, ext;

    CHECK(mgmt_auth() == 0, "management key (external authenticate)");

    /* Import a key (slow path: expect a time extension first). */
    static const uint8_t d[48] = {
        0x6b, 0x9d, 0x3d, 0xad, 0x2e, 0x1b, 0x8c, 0x1c, 0x05, 0xb1, 0x98, 0x75, 0xb6, 0x65, 0x9f, 0x4d,
        0xe2, 0x3c, 0x3b, 0x66, 0x7b, 0xf2, 0x97, 0xba, 0x9a, 0xa4, 0x77, 0x40, 0x78, 0x71, 0x37, 0xd8,
        0x96, 0xd5, 0x72, 0x4e, 0x4c, 0x70, 0xa8, 0x25, 0xf8, 0x72, 0xc9, 0xea, 0x60, 0xd2, 0xed, 0xf5};
    memcpy(cmd, "\x00\xfe\x14\x9e\x35\x06\x30", 7);
    memcpy(cmd + 7, d, 48);
    memcpy(cmd + 55, "\xaa\x01\x01", 3);
    r = xfr(cmd, 58, resp, &ext);
    CHECK(r == 2 && sw(resp, r) == 0x9000 && ext == 1, "import key with a time extension (ext=%d)", ext);

    /* Sign with 9E (PIN policy never). */
    memcpy(cmd, "\x00\x87\x14\x9e\x36\x7c\x34\x82\x00\x81\x30", 11);
    memset(cmd + 11, 0x5a, 48);
    r = xfr(cmd, 59, resp, &ext);
    CHECK(r > 10 && sw(resp, r) == 0x9000 && resp[0] == 0x7c && resp[2] == 0x82 && resp[4] == 0x30 && ext == 1,
          "sign over CCID");

    /* Store a 52-byte object so GET DATA returns 54 bytes: a 64-byte reply needing a ZLP. */
    memcpy(cmd, "\x00\xdb\x3f\xff\x39\x5c\x03\x5f\xc1\x07\x53\x32", 12);
    memset(cmd + 12, 0x11, 50);
    r = xfr(cmd, 62, resp, NULL);
    CHECK(sw(resp, r) == 0x9000, "PUT DATA small object");
    memcpy(cmd, "\x00\xcb\x3f\xff\x05\x5c\x03\x5f\xc1\x07\x00", 11);
    r = xfr(cmd, 11, resp, NULL);
    CHECK(r == 54 && sw(resp, r) == 0x9000, "64-byte reply terminated by a ZLP (%d)", r);

    /* A 600-byte object: chained PUT DATA, 61xx + GET RESPONSE to read back. */
    static uint8_t obj[612];
    size_t olen = 0;
    memcpy(obj, "\x5c\x03\x5f\xc1\x05\x53\x82\x02\x58", 9);
    olen = 9;
    for (int i = 0; i < 600; i++)
        obj[olen++] = (uint8_t)i;
    for (size_t off = 0; off < olen;) {
        size_t n = olen - off > 255 ? 255 : olen - off;
        cmd[0] = off + n < olen ? 0x10 : 0x00;
        memcpy(cmd + 1, "\xdb\x3f\xff", 3);
        cmd[4] = (uint8_t)n;
        memcpy(cmd + 5, obj + off, n);
        r = xfr(cmd, 5 + n, resp, NULL);
        CHECK(sw(resp, r) == 0x9000, "chained PUT DATA at %zu", off);
        off += n;
    }
    memcpy(cmd, "\x00\xcb\x3f\xff\x05\x5c\x03\x5f\xc1\x05\x00", 11);
    static uint8_t got[700];
    size_t glen = 0;
    r = xfr(cmd, 11, resp, NULL);
    while (r >= 2) {
        memcpy(got + glen, resp, (size_t)r - 2);
        glen += (size_t)r - 2;
        if (resp[r - 2] != 0x61)
            break;
        uint8_t gr[5] = {0x00, 0xc0, 0x00, 0x00, resp[r - 1]};
        r = xfr(gr, 5, resp, NULL);
    }
    CHECK(glen == olen - 5 && memcmp(got, obj + 5, glen) == 0 && sw(resp, r) == 0x9000,
          "read back 600-byte object through GET RESPONSE");
}

static void test_halt_and_reset(void)
{
    uint8_t resp[400], buf[8];
    static const uint8_t select[] = {0x00, 0xa4, 0x04, 0x00, 0x05, 0xa0, 0x00, 0x00, 0x03, 0x08};
    int r;

    /* Halt and clear the bulk IN endpoint: toggles restart at DATA0. */
    r = host_control(&dev, 0x02, 3, 0, 0x81, NULL, 0);
    CHECK(r == 0, "SET_FEATURE(HALT) on EP1 IN");
    r = host_control(&dev, 0x82, 0, 0, 0x81, buf, 2);
    CHECK(r == 2 && buf[0] == 1, "GET_STATUS shows the halt");
    int toggle;
    CHECK(mock_in(dev.addr, 1, resp, 64, &toggle) == MOCK_STALL, "halted endpoint stalls");
    r = host_control(&dev, 0x02, 1, 0, 0x81, NULL, 0);
    CHECK(r == 0, "CLEAR_FEATURE(HALT) on EP1 IN");
    dev.toggle_in[1] = 0;
    r = host_control(&dev, 0x02, 1, 0, 0x01, NULL, 0);
    CHECK(r == 0, "CLEAR_FEATURE(HALT) on EP1 OUT");
    dev.toggle_out[1] = 0;
    r = xfr(select, sizeof(select), resp, NULL);
    CHECK(r == 42, "CCID works after clearing halts");

    /* SET_INTERFACE restarts the interface's data toggles at DATA0. */
    if (!dev.toggle_out[1] || !dev.toggle_in[1])
        xfr(select, sizeof(select), resp, NULL);
    CHECK(dev.toggle_out[1] && dev.toggle_in[1], "toggles at DATA1 before SET_INTERFACE");
    r = host_control(&dev, 0x01, 0x0b, 0, 0, NULL, 0);
    CHECK(r == 0, "SET_INTERFACE(0, 0)");
    dev.toggle_in[1] = dev.toggle_out[1] = 0;
    r = xfr(select, sizeof(select), resp, NULL);
    CHECK(r == 42, "CCID works after SET_INTERFACE");

    r = ccid(0x72, 0, NULL, 0, resp, sizeof(resp), NULL);
    CHECK(r == 10 && resp[0] == 0x81 && resp[7] == 0x00, "PC_to_RDR_Abort succeeds");

    /* Bus reset and re-enumerate: the device must come back. */
    uint8_t d[64], c[128];
    r = host_enumerate(&dev, 9, d, c, sizeof(c));
    CHECK(r == 93, "re-enumeration");
    r = ccid(0x62, 0, NULL, 0, resp, sizeof(resp), NULL);
    CHECK(r == 10 + (int)piv_atr_len, "power on after re-enumeration");
    r = xfr(select, sizeof(select), resp, NULL);
    CHECK(r == 42, "SELECT after re-enumeration");
}

int main(void)
{
    uint8_t d[64], c[128];
    int r;

    host_flash_open(NULL);
    piv_init();
    mock_start_device_thread();

    r = host_enumerate(&dev, 5, d, c, sizeof(c));
    CHECK(r == 93, "enumeration (%d)", r);
    if (r < 0)
        return 1;
    test_descriptors(d, c, r);
    test_control();
    test_interrupt();
    test_ccid_basics();
    test_ccid_long();
    test_halt_and_reset();

    if (failures) {
        printf("usb: %d failures\n", failures);
        return 1;
    }
    printf("usb: all tests passed\n");
    return 0;
}
