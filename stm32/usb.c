/*
 * Driver for the STM32F103 full-speed USB device peripheral, presenting a
 * single CCID interface:
 *
 *   EP0       control
 *   EP1 IN    bulk, CCID responses        (0x81)
 *   EP1 OUT   bulk, CCID commands         (0x01)
 *   EP2 IN    interrupt, slot change      (0x82)
 *
 * Packet memory (PMA) is 512 bytes of 16-bit words; on the F103 each word sits
 * in a 32-bit slot, which the pma_rd()/pma_wr() accessors hide.
 */
#include "usb.h"

#include <string.h>

#include "../src/ccid.h"
#include "../src/piv.h"

#ifdef USB_HW_MOCK
#include "../test/usb_mock.h"
#else
#include "stm32f103.h"
#define rd(reg) ((uint16_t)REG32(USB_BASE + (reg)))
#define wr(reg, v) (REG32(USB_BASE + (reg)) = (uint16_t)(v))
#define pma_rd(off) ((uint16_t)REG32(USB_PMA_BASE + 2 * (off)))
#define pma_wr(off, v) (REG32(USB_PMA_BASE + 2 * (off)) = (uint16_t)(v))
#define irq_off() __asm__ volatile("cpsid i" ::: "memory")
#define irq_on() __asm__ volatile("cpsie i" ::: "memory")
#endif

#ifndef USB_VID
#define USB_VID 0x1209 /* pid.codes */
#endif
#ifndef USB_PID
#define USB_PID 0x0001 /* pid.codes test PID: fine for a private device */
#endif

/* Register offsets */
#define EPR(n) (4 * (n))
#define CNTR 0x40
#define ISTR 0x44
#define DADDR 0x4c
#define BTABLE 0x50

#define CNTR_CTRM 0x8000
#define CNTR_RESETM 0x0400
#define CNTR_PDWN 0x0002
#define CNTR_FRES 0x0001

#define ISTR_CTR 0x8000
#define ISTR_RESET 0x0400
#define ISTR_EP_ID 0x000f
#define ISTR_IGNORED 0x7b00 /* PMAOVR ERR WKUP SUSP SOF ESOF */

#define DADDR_EF 0x80

#define EP_CTR_RX 0x8000
#define EP_DTOG_RX 0x4000
#define EP_STAT_RX 0x3000
#define EP_SETUP 0x0800
#define EP_TYPE 0x0600
#define EP_KIND 0x0100
#define EP_CTR_TX 0x0080
#define EP_DTOG_TX 0x0040
#define EP_STAT_TX 0x0030
#define EP_EA 0x000f
#define EP_RW (EP_TYPE | EP_KIND | EP_EA) /* plain read/write bits */

#define EP_BULK 0x0000
#define EP_CONTROL 0x0200
#define EP_INTERRUPT 0x0600

#define TX_DISABLED 0x0000
#define TX_STALL 0x0010
#define TX_NAK 0x0020
#define TX_VALID 0x0030
#define RX_DISABLED 0x0000
#define RX_STALL 0x1000
#define RX_NAK 0x2000
#define RX_VALID 0x3000

/* PMA layout: buffer descriptor table, then packet buffers. */
#define BT_ADDR_TX(n) (8 * (n) + 0)
#define BT_COUNT_TX(n) (8 * (n) + 2)
#define BT_ADDR_RX(n) (8 * (n) + 4)
#define BT_COUNT_RX(n) (8 * (n) + 6)
#define EP0_TX_BUF 0x040
#define EP0_RX_BUF 0x080
#define EP1_TX_BUF 0x0c0
#define EP1_RX_BUF 0x100
#define EP2_TX_BUF 0x140
#define RX_64_BYTES 0x8400 /* BL_SIZE=1, NUM_BLOCK=1 */

#define EP0_SIZE 64
#define BULK_SIZE 64
#define BULK_EP 1
#define INT_EP 2

static const uint8_t device_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, EP0_SIZE,
    USB_VID & 0xff, USB_VID >> 8, USB_PID & 0xff, USB_PID >> 8,
    0x00, 0x01, 1, 2, 3, 1,
};

#define CONFIG_LEN (9 + 9 + 54 + 7 + 7 + 7)
#define LE32(x) (x) & 0xff, ((x) >> 8) & 0xff, ((x) >> 16) & 0xff, ((x) >> 24) & 0xff

static const uint8_t config_desc[CONFIG_LEN] = {
    9, 2, CONFIG_LEN, 0, 1, 1, 0, 0x80, 50, /* bus powered, 100 mA */
    9, 4, 0, 0, 3, 0x0b, 0, 0, 0,           /* interface 0: CCID */
    54, 0x21,
    0x10, 0x01,          /* bcdCCID 1.10 */
    0,                   /* bMaxSlotIndex */
    0x07,                /* bVoltageSupport: 5V, 3V, 1.8V */
    LE32(0x00000002),    /* dwProtocols: T=1 */
    LE32(4000),          /* dwDefaultClock (kHz) */
    LE32(4000),          /* dwMaximumClock */
    0,                   /* bNumClockSupported */
    LE32(10752),         /* dwDataRate (bps) */
    LE32(10752),         /* dwMaxDataRate */
    0,                   /* bNumDataRatesSupported */
    LE32(254),           /* dwMaxIFSD */
    LE32(0),             /* dwSynchProtocols */
    LE32(0),             /* dwMechanical */
    LE32(0x000200fe),    /* dwFeatures: automatic everything, short APDU level */
    LE32(CCID_MAX_MSG),  /* dwMaxCCIDMessageLength */
    0xff,                /* bClassGetResponse: echo */
    0xff,                /* bClassEnvelope: echo */
    0, 0,                /* wLcdLayout */
    0,                   /* bPINSupport */
    1,                   /* bMaxCCIDBusySlots */
    7, 5, 0x80 | BULK_EP, 0x02, BULK_SIZE, 0, 0,
    7, 5, BULK_EP, 0x02, BULK_SIZE, 0, 0,
    7, 5, 0x80 | INT_EP, 0x03, 8, 0, 255,
};

static const uint8_t lang_desc[4] = {4, 3, 0x09, 0x04};

static uint8_t string_desc[2 + 2 * 16];

enum { CTL_IDLE, CTL_DATA_IN, CTL_STATUS_IN, CTL_STATUS_OUT };

static struct {
    uint8_t state;
    uint8_t zlp;
    uint8_t new_addr;
    const uint8_t *ptr;
    uint16_t left;
} ctl;

static volatile uint8_t configured;
static volatile uint8_t bulk_busy;
static uint8_t bulk_zlp;
static const uint8_t *bulk_ptr;
static size_t bulk_left;

static void pma_write(unsigned off, const uint8_t *src, unsigned n)
{
    for (unsigned i = 0; i < n; i += 2)
        pma_wr(off + i, src[i] | (i + 1 < n ? src[i + 1] << 8 : 0));
}

static void pma_read(uint8_t *dst, unsigned off, unsigned n)
{
    for (unsigned i = 0; i < n; i += 2) {
        uint16_t w = pma_rd(off + i);
        dst[i] = (uint8_t)w;
        if (i + 1 < n)
            dst[i + 1] = (uint8_t)(w >> 8);
    }
}

/* STAT and DTOG bits toggle when written with 1, so write the difference. */
static void set_stat_tx(unsigned ep, uint16_t stat)
{
    uint16_t v = rd(EPR(ep));
    wr(EPR(ep), ((v & (EP_RW | EP_STAT_TX)) ^ stat) | EP_CTR_RX | EP_CTR_TX);
}

static void set_stat_rx(unsigned ep, uint16_t stat)
{
    uint16_t v = rd(EPR(ep));
    wr(EPR(ep), ((v & (EP_RW | EP_STAT_RX)) ^ stat) | EP_CTR_RX | EP_CTR_TX);
}

static void set_stat(unsigned ep, uint16_t tx, uint16_t rx)
{
    uint16_t v = rd(EPR(ep));
    wr(EPR(ep), ((v & (EP_RW | EP_STAT_TX | EP_STAT_RX)) ^ (tx | rx)) | EP_CTR_RX | EP_CTR_TX);
}

/* Sets type and address, clears both data toggles and both CTR flags. */
static void ep_setup(unsigned ep, uint16_t type, uint16_t tx, uint16_t rx)
{
    uint16_t v = rd(EPR(ep));
    wr(EPR(ep), type | ep | (v & (EP_DTOG_RX | EP_DTOG_TX)) |
                    ((v & EP_STAT_TX) ^ tx) | ((v & EP_STAT_RX) ^ rx));
}

static void clear_ctr_rx(unsigned ep)
{
    wr(EPR(ep), (rd(EPR(ep)) & EP_RW) | EP_CTR_TX);
}

static void clear_ctr_tx(unsigned ep)
{
    wr(EPR(ep), (rd(EPR(ep)) & EP_RW) | EP_CTR_RX);
}

static void bulk_send_next(void)
{
    unsigned n = bulk_left > BULK_SIZE ? BULK_SIZE : (unsigned)bulk_left;

    pma_write(EP1_TX_BUF, bulk_ptr, n);
    pma_wr(BT_COUNT_TX(BULK_EP), n);
    bulk_ptr += n;
    bulk_left -= n;
    set_stat_tx(BULK_EP, TX_VALID);
}

int usb_bulk_busy(void)
{
    return bulk_busy;
}

void usb_bulk_send(const uint8_t *data, size_t len)
{
    irq_off();
    if (configured) {
        bulk_ptr = data;
        bulk_left = len;
        bulk_zlp = len && len % BULK_SIZE == 0;
        bulk_busy = 1;
        bulk_send_next();
    }
    irq_on();
}

void usb_bulk_out_enable(void)
{
    irq_off();
    if (configured)
        set_stat_rx(BULK_EP, RX_VALID);
    irq_on();
}

static void ep0_send_next(void)
{
    unsigned n = ctl.left > EP0_SIZE ? EP0_SIZE : ctl.left;

    pma_write(EP0_TX_BUF, ctl.ptr, n);
    pma_wr(BT_COUNT_TX(0), n);
    ctl.ptr += n;
    ctl.left -= n;
    set_stat_tx(0, TX_VALID);
}

static void ep0_reply(const uint8_t *data, unsigned len, unsigned wlength)
{
    if (len > wlength)
        len = wlength;
    ctl.ptr = data;
    ctl.left = (uint16_t)len;
    /* A short final packet ends the data stage; a full one needs a ZLP. */
    ctl.zlp = len < wlength && len % EP0_SIZE == 0;
    ctl.state = CTL_DATA_IN;
    ep0_send_next();
    set_stat_rx(0, RX_VALID); /* for the status stage (or an early abort) */
}

static void ep0_status(void)
{
    pma_wr(BT_COUNT_TX(0), 0);
    ctl.state = CTL_STATUS_IN;
    set_stat(0, TX_VALID, RX_VALID);
}

static void ep0_stall(void)
{
    ctl.state = CTL_IDLE;
    set_stat(0, TX_STALL, RX_STALL);
}

static const uint8_t *string_for(unsigned idx, unsigned *len)
{
    static const char *const strs[] = {"fore", "fore PIV"};
    char tmp[16];
    const char *s;
    unsigned n = 0;

    if (idx == 0) {
        *len = sizeof(lang_desc);
        return lang_desc;
    }
    if (idx == 1 || idx == 2) {
        s = strs[idx - 1];
    } else if (idx == 3) {
        uint32_t serial = piv_serial();
        for (int i = 7; i >= 0; i--)
            tmp[n++] = "0123456789ABCDEF"[(serial >> (4 * i)) & 15];
        tmp[n] = 0;
        s = tmp;
    } else {
        return NULL;
    }
    for (n = 0; s[n] && n < 16; n++) {
        string_desc[2 + 2 * n] = (uint8_t)s[n];
        string_desc[3 + 2 * n] = 0;
    }
    string_desc[0] = (uint8_t)(2 + 2 * n);
    string_desc[1] = 3;
    *len = string_desc[0];
    return string_desc;
}

static void configure(uint8_t value)
{
    configured = 0;
    bulk_busy = 0;
    ccid_usb_reset();
    if (value) {
        static const uint8_t notify[2] = {0x50, 0x03}; /* NotifySlotChange: card present, changed */
        ep_setup(BULK_EP, EP_BULK, TX_NAK, RX_VALID);
        ep_setup(INT_EP, EP_INTERRUPT, TX_NAK, RX_DISABLED);
        pma_write(EP2_TX_BUF, notify, 2);
        pma_wr(BT_COUNT_TX(INT_EP), 2);
        set_stat_tx(INT_EP, TX_VALID);
        configured = value;
    } else {
        ep_setup(BULK_EP, EP_BULK, TX_DISABLED, RX_DISABLED);
        ep_setup(INT_EP, EP_INTERRUPT, TX_DISABLED, RX_DISABLED);
    }
}

static int ep_halt(unsigned addr, int halt)
{
    unsigned ep = addr & 0x7f;

    if (!configured || ep == 0 || ep > INT_EP || (ep == INT_EP && !(addr & 0x80)))
        return -1;
    if (addr & 0x80) {
        uint16_t v = rd(EPR(ep));
        if (halt) {
            set_stat_tx(ep, TX_STALL);
        } else {
            /* Clearing a halt resets the data toggle. */
            wr(EPR(ep), (v & EP_RW) | (v & EP_DTOG_TX) | EP_CTR_RX | EP_CTR_TX);
            set_stat_tx(ep, ep == BULK_EP && bulk_busy ? TX_VALID : TX_NAK);
        }
    } else {
        uint16_t v = rd(EPR(ep));
        if (halt) {
            set_stat_rx(ep, RX_STALL);
        } else {
            wr(EPR(ep), (v & EP_RW) | (v & EP_DTOG_RX) | EP_CTR_RX | EP_CTR_TX);
            set_stat_rx(ep, RX_VALID);
        }
    }
    return 0;
}

static void handle_setup(void)
{
    static uint8_t buf[2];
    uint8_t s[8];
    unsigned type, req, value, index, length, len = 0;
    const uint8_t *p = NULL;

    pma_read(s, EP0_RX_BUF, 8);
    type = s[0];
    req = s[1];
    value = s[2] | s[3] << 8;
    index = s[4] | s[5] << 8;
    length = s[6] | s[7] << 8;
    ctl.state = CTL_IDLE;

    if ((type & 0x60) == 0x20) {
        /* CCID class requests: accept ABORT, nothing else. */
        if (type == 0x21 && req == 0x01 && index == 0 && length == 0)
            ep0_status();
        else
            ep0_stall();
        return;
    }
    if ((type & 0x60) != 0) {
        ep0_stall();
        return;
    }

    switch (req) {
    case 0x00: /* GET_STATUS */
        buf[0] = buf[1] = 0;
        if ((type & 0x1f) == 2) {
            unsigned ep = index & 0x7f;
            if (ep > INT_EP) {
                ep0_stall();
                return;
            }
            uint16_t v = rd(EPR(ep));
            buf[0] = (index & 0x80) ? (v & EP_STAT_TX) == TX_STALL : (v & EP_STAT_RX) == RX_STALL;
        }
        ep0_reply(buf, 2, length);
        return;
    case 0x01: /* CLEAR_FEATURE */
    case 0x03: /* SET_FEATURE */
        if (type == 0x02 && value == 0 && ep_halt(index, req == 0x03) == 0)
            ep0_status();
        else
            ep0_stall();
        return;
    case 0x05: /* SET_ADDRESS: takes effect after the status stage */
        ctl.new_addr = value & 0x7f;
        ep0_status();
        return;
    case 0x06: /* GET_DESCRIPTOR */
        switch (value >> 8) {
        case 1:
            p = device_desc;
            len = sizeof(device_desc);
            break;
        case 2:
            p = config_desc;
            len = sizeof(config_desc);
            break;
        case 3:
            p = string_for(value & 0xff, &len);
            break;
        }
        if (p)
            ep0_reply(p, len, length);
        else
            ep0_stall();
        return;
    case 0x08: /* GET_CONFIGURATION */
        buf[0] = configured;
        ep0_reply(buf, 1, length);
        return;
    case 0x09: /* SET_CONFIGURATION */
        if (value > 1) {
            ep0_stall();
            return;
        }
        configure((uint8_t)value);
        ep0_status();
        return;
    case 0x0a: /* GET_INTERFACE */
        buf[0] = 0;
        if (configured && index == 0)
            ep0_reply(buf, 1, length);
        else
            ep0_stall();
        return;
    case 0x0b: /* SET_INTERFACE */
        if (configured && index == 0 && value == 0)
            ep0_status();
        else
            ep0_stall();
        return;
    }
    ep0_stall();
}

static void ep0_in_done(void)
{
    switch (ctl.state) {
    case CTL_DATA_IN:
        if (ctl.left) {
            ep0_send_next();
        } else if (ctl.zlp) {
            ctl.zlp = 0;
            ep0_send_next();
        } else {
            ctl.state = CTL_STATUS_OUT;
        }
        break;
    case CTL_STATUS_IN:
        if (ctl.new_addr) {
            wr(DADDR, DADDR_EF | ctl.new_addr);
            ctl.new_addr = 0;
        }
        ctl.state = CTL_IDLE;
        break;
    }
}

static void bus_reset(void)
{
    wr(BTABLE, 0);
    pma_wr(BT_ADDR_TX(0), EP0_TX_BUF);
    pma_wr(BT_COUNT_TX(0), 0);
    pma_wr(BT_ADDR_RX(0), EP0_RX_BUF);
    pma_wr(BT_COUNT_RX(0), RX_64_BYTES);
    pma_wr(BT_ADDR_TX(BULK_EP), EP1_TX_BUF);
    pma_wr(BT_COUNT_TX(BULK_EP), 0);
    pma_wr(BT_ADDR_RX(BULK_EP), EP1_RX_BUF);
    pma_wr(BT_COUNT_RX(BULK_EP), RX_64_BYTES);
    pma_wr(BT_ADDR_TX(INT_EP), EP2_TX_BUF);
    pma_wr(BT_COUNT_TX(INT_EP), 0);
    pma_wr(BT_ADDR_RX(INT_EP), 0);
    pma_wr(BT_COUNT_RX(INT_EP), 0);

    ep_setup(0, EP_CONTROL, TX_NAK, RX_VALID);
    configure(0);
    ctl.state = CTL_IDLE;
    ctl.new_addr = 0;
    wr(DADDR, DADDR_EF);
}

void usb_irq(void)
{
    uint16_t istr = rd(ISTR);

    if (istr & ISTR_RESET) {
        wr(ISTR, (uint16_t)~ISTR_RESET);
        bus_reset();
        return;
    }
    while ((istr = rd(ISTR)) & ISTR_CTR) {
        unsigned ep = istr & ISTR_EP_ID;
        uint16_t v = rd(EPR(ep));

        if (v & EP_CTR_TX) {
            clear_ctr_tx(ep);
            if (ep == 0) {
                ep0_in_done();
            } else if (ep == BULK_EP && bulk_busy) {
                if (bulk_left) {
                    bulk_send_next();
                } else if (bulk_zlp) {
                    bulk_zlp = 0;
                    bulk_send_next();
                } else {
                    bulk_busy = 0;
                }
            }
        }
        if (v & EP_CTR_RX) {
            clear_ctr_rx(ep);
            if (ep == 0) {
                if (v & EP_SETUP) {
                    handle_setup();
                } else {
                    /* Status stage of an IN transfer (or data we never asked for). */
                    ctl.state = CTL_IDLE;
                    set_stat_rx(0, RX_VALID);
                }
            } else if (ep == BULK_EP) {
                uint8_t pkt[BULK_SIZE];
                unsigned n = pma_rd(BT_COUNT_RX(BULK_EP)) & 0x3ff;
                if (n > BULK_SIZE)
                    n = BULK_SIZE;
                pma_read(pkt, EP1_RX_BUF, n);
                if (ccid_out_packet(pkt, n))
                    set_stat_rx(BULK_EP, RX_VALID);
            }
        }
    }
    /* Drop flags for events we leave masked (suspend, wakeup, SOF, errors). */
    wr(ISTR, (uint16_t)~ISTR_IGNORED);
}

void usb_init(void)
{
    wr(CNTR, CNTR_FRES); /* power up the transceiver, hold the reset */
    for (volatile int i = 0; i < 100; i++)
        ; /* t_STARTUP is 1 us */
    wr(CNTR, 0);
    wr(ISTR, 0);
    wr(CNTR, CNTR_CTRM | CNTR_RESETM);
}
