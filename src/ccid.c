#include "ccid.h"

#include <string.h>

#include "piv.h"

#define PC_TO_RDR_SET_PARAMETERS 0x61
#define PC_TO_RDR_ICC_POWER_ON 0x62
#define PC_TO_RDR_ICC_POWER_OFF 0x63
#define PC_TO_RDR_GET_SLOT_STATUS 0x65
#define PC_TO_RDR_ESCAPE 0x6b
#define PC_TO_RDR_GET_PARAMETERS 0x6c
#define PC_TO_RDR_RESET_PARAMETERS 0x6d
#define PC_TO_RDR_SET_DATA_RATE 0x6e
#define PC_TO_RDR_XFR_BLOCK 0x6f

#define RDR_TO_PC_DATA_BLOCK 0x80
#define RDR_TO_PC_SLOT_STATUS 0x81
#define RDR_TO_PC_PARAMETERS 0x82
#define RDR_TO_PC_ESCAPE 0x83
#define RDR_TO_PC_DATA_RATE 0x84

/* bStatus = bmCommandStatus (bits 7..6) | bmICCStatus (bits 1..0) */
#define ICC_ACTIVE 0x00
#define ICC_INACTIVE 0x01
#define ICC_ABSENT 0x02
#define CMD_FAILED 0x40
#define CMD_TIME_EXTENSION 0x80

/* bError */
#define ERR_CMD_NOT_SUPPORTED 0x00
#define ERR_BAD_LENGTH 0x01
#define ERR_BAD_SLOT 0x05
#define ERR_ICC_MUTE 0xfe

#define CLOCK_KHZ 4000
#define DATA_RATE 10752

/* T=1 protocol data for (Get|Set|Reset)Parameters. */
static const uint8_t T1_PARAMS[7] = {0x11, 0x10, 0x00, 0x4d, 0x00, 0xfe, 0x00};

static uint8_t rx[CCID_MAX_MSG];
static volatile size_t rx_len; /* bytes of the current message seen, stored or not */
static volatile uint8_t rx_ready;
static uint8_t tx[CCID_HDR_LEN + PIV_MAX_RESPONSE];
static uint8_t powered;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int ccid_out_packet(const uint8_t *pkt, size_t len)
{
    size_t have = rx_len;

    if (rx_ready)
        return 0;
    if (have < sizeof(rx))
        memcpy(rx + have, pkt, len < sizeof(rx) - have ? len : sizeof(rx) - have);
    have += len;
    rx_len = have;
    /* A message ends once dwLength bytes have arrived, or at a short packet. */
    if ((have >= CCID_HDR_LEN && have >= CCID_HDR_LEN + (uint64_t)le32(rx + 1)) ||
        len < CCID_PACKET)
        rx_ready = 1;
    return !rx_ready;
}

void ccid_usb_reset(void)
{
    rx_len = 0;
    rx_ready = 0;
}

static void reply(uint8_t type, uint8_t status, uint8_t error, uint8_t b9, size_t len)
{
    tx[0] = type;
    tx[1] = (uint8_t)len;
    tx[2] = (uint8_t)(len >> 8);
    tx[3] = 0;
    tx[4] = 0;
    tx[5] = rx[5];
    tx[6] = rx[6];
    tx[7] = status;
    tx[8] = error;
    tx[9] = b9;
    usb_bulk_send(tx, CCID_HDR_LEN + len);
}

static uint8_t reply_type(uint8_t cmd)
{
    switch (cmd) {
    case PC_TO_RDR_ICC_POWER_ON:
    case PC_TO_RDR_XFR_BLOCK:
        return RDR_TO_PC_DATA_BLOCK;
    case PC_TO_RDR_SET_PARAMETERS:
    case PC_TO_RDR_GET_PARAMETERS:
    case PC_TO_RDR_RESET_PARAMETERS:
        return RDR_TO_PC_PARAMETERS;
    case PC_TO_RDR_ESCAPE:
        return RDR_TO_PC_ESCAPE;
    case PC_TO_RDR_SET_DATA_RATE:
        return RDR_TO_PC_DATA_RATE;
    default:
        return RDR_TO_PC_SLOT_STATUS;
    }
}

static void handle(size_t len)
{
    uint8_t cmd = rx[0], rtype = reply_type(cmd);
    uint8_t icc = powered ? ICC_ACTIVE : ICC_INACTIVE;
    size_t dlen = len - CCID_HDR_LEN, n;

    if (len > sizeof(rx) || le32(rx + 1) != dlen) {
        reply(rtype, CMD_FAILED | icc, ERR_BAD_LENGTH, 0, 0);
        return;
    }
    if (rx[5] != 0) {
        reply(rtype, CMD_FAILED | ICC_ABSENT, ERR_BAD_SLOT, 0, 0);
        return;
    }

    switch (cmd) {
    case PC_TO_RDR_ICC_POWER_ON:
        piv_reset();
        powered = 1;
        memcpy(tx + CCID_HDR_LEN, piv_atr, piv_atr_len);
        reply(rtype, ICC_ACTIVE, 0, 0, piv_atr_len);
        break;
    case PC_TO_RDR_ICC_POWER_OFF:
        piv_reset();
        powered = 0;
        reply(rtype, ICC_INACTIVE, 0, 0, 0);
        break;
    case PC_TO_RDR_GET_SLOT_STATUS:
        reply(rtype, icc, 0, 0, 0);
        break;
    case PC_TO_RDR_XFR_BLOCK:
        if (!powered) {
            reply(rtype, CMD_FAILED | ICC_INACTIVE, ERR_ICC_MUTE, 0, 0);
            break;
        }
        if (piv_is_slow(rx + CCID_HDR_LEN, dlen)) {
            /* Ask the host to keep waiting while we do the slow part. */
            reply(rtype, CMD_TIME_EXTENSION | ICC_ACTIVE, 1, 0, 0);
            while (usb_bulk_busy())
                ;
        }
        platform_activity(1);
        n = piv_apdu(rx + CCID_HDR_LEN, dlen, tx + CCID_HDR_LEN);
        platform_activity(0);
        reply(rtype, ICC_ACTIVE, 0, 0, n);
        break;
    case PC_TO_RDR_SET_PARAMETERS:
    case PC_TO_RDR_GET_PARAMETERS:
    case PC_TO_RDR_RESET_PARAMETERS:
        /* Parameters are fixed; report them whatever was asked for. */
        memcpy(tx + CCID_HDR_LEN, T1_PARAMS, sizeof(T1_PARAMS));
        reply(rtype, icc, 0, 1 /* T=1 */, sizeof(T1_PARAMS));
        break;
    case PC_TO_RDR_SET_DATA_RATE:
        tx[CCID_HDR_LEN + 0] = (uint8_t)CLOCK_KHZ;
        tx[CCID_HDR_LEN + 1] = (uint8_t)(CLOCK_KHZ >> 8);
        tx[CCID_HDR_LEN + 2] = 0;
        tx[CCID_HDR_LEN + 3] = 0;
        tx[CCID_HDR_LEN + 4] = (uint8_t)DATA_RATE;
        tx[CCID_HDR_LEN + 5] = (uint8_t)(DATA_RATE >> 8);
        tx[CCID_HDR_LEN + 6] = 0;
        tx[CCID_HDR_LEN + 7] = 0;
        reply(rtype, icc, 0, 0, 8);
        break;
    default:
        reply(rtype, CMD_FAILED | icc, ERR_CMD_NOT_SUPPORTED, 0, 0);
        break;
    }
}

void ccid_poll(void)
{
    if (!rx_ready || usb_bulk_busy())
        return;
    if (rx_len >= CCID_HDR_LEN)
        handle(rx_len);
    rx_len = 0;
    rx_ready = 0;
    usb_bulk_out_enable();
}
