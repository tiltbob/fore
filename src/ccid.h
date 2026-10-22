/*
 * USB CCID (smart card reader class) front end for the PIV applet: one slot
 * with a permanently inserted card, short-APDU level exchanges.
 */
#ifndef CCID_H
#define CCID_H

#include <stddef.h>
#include <stdint.h>

#define CCID_HDR_LEN 10
/* Longest short APDU (4 + 1 + 255 + 1) plus the CCID header. */
#define CCID_MAX_MSG (CCID_HDR_LEN + 261)
#define CCID_PACKET 64

/* Bulk OUT packet from the host (interrupt context). Returns 1 if the driver
 * should accept the next packet now, 0 to NAK until ccid_poll() re-enables. */
int ccid_out_packet(const uint8_t *pkt, size_t len);
/* Main loop: handles a complete command message, if any. */
void ccid_poll(void);
/* USB bus reset or (de)configuration: drops any partial message. */
void ccid_usb_reset(void);

/* Provided by the USB driver. */
int usb_bulk_busy(void);
/* Queues a bulk IN transfer (terminated with a ZLP when needed). */
void usb_bulk_send(const uint8_t *data, size_t len);
void usb_bulk_out_enable(void);

/* Provided by the platform: activity indicator (LED). */
void platform_activity(int on);

#endif
