/*
 * A stand-in for libusb-1.0, LD_PRELOADed into pcscd, that connects the real
 * libccid driver to the firmware: stm32/usb.c runs against the peripheral
 * model (usb_mock.c) with src/ccid.c and the applet behind it, and a thread
 * plays the firmware main loop. Only what libccid uses is implemented.
 *
 * FORE_FLASH names the file that backs the simulated flash.
 */
#include <libusb-1.0/libusb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../host/platform_host.h"
#include "../src/piv.h"
#include "usb_mock.h"

#define API __attribute__((visibility("default")))
#define DEV_ADDR 7

struct libusb_context {
    int unused;
};
struct libusb_device {
    int unused;
};
struct libusb_device_handle {
    int unused;
};

static struct libusb_context the_ctx;
static struct libusb_device the_dev;
static struct libusb_device_handle the_handle;

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static usb_host_dev host;
static uint8_t dev_desc[64], cfg_desc[256];
static int cfg_len;

static struct libusb_transfer *pending;
static int pending_cancelled;
static long pending_deadline;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static void bring_up(void)
{
    host_flash_open(getenv("FORE_FLASH"));
    piv_init();
    mock_start_device_thread();
    cfg_len = host_enumerate(&host, DEV_ADDR, dev_desc, cfg_desc, sizeof(cfg_desc));
    if (cfg_len < 0)
        abort();
}

API int libusb_init(libusb_context **ctx)
{
    pthread_once(&once, bring_up);
    if (ctx)
        *ctx = &the_ctx;
    return 0;
}

API void libusb_exit(libusb_context *ctx)
{
    (void)ctx;
}

API const char *libusb_error_name(int code)
{
    switch (code) {
    case LIBUSB_SUCCESS: return "LIBUSB_SUCCESS";
    case LIBUSB_ERROR_IO: return "LIBUSB_ERROR_IO";
    case LIBUSB_ERROR_TIMEOUT: return "LIBUSB_ERROR_TIMEOUT";
    case LIBUSB_ERROR_PIPE: return "LIBUSB_ERROR_PIPE";
    case LIBUSB_ERROR_NOT_FOUND: return "LIBUSB_ERROR_NOT_FOUND";
    default: return "LIBUSB_ERROR_OTHER";
    }
}

API ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    (void)ctx;
    *list = calloc(2, sizeof(**list));
    (*list)[0] = &the_dev;
    return 1;
}

API void libusb_free_device_list(libusb_device **list, int unref)
{
    (void)unref;
    free(list);
}

API uint8_t libusb_get_bus_number(libusb_device *dev)
{
    (void)dev;
    return 1;
}

API uint8_t libusb_get_device_address(libusb_device *dev)
{
    (void)dev;
    return DEV_ADDR;
}

API int libusb_get_port_numbers(libusb_device *dev, uint8_t *ports, int len)
{
    (void)dev;
    if (len < 1)
        return LIBUSB_ERROR_OVERFLOW;
    ports[0] = 1;
    return 1;
}

API libusb_device *libusb_get_device(libusb_device_handle *h)
{
    (void)h;
    return &the_dev;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

API int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *d)
{
    const uint8_t *p = dev_desc;
    (void)dev;
    d->bLength = p[0];
    d->bDescriptorType = p[1];
    d->bcdUSB = le16(p + 2);
    d->bDeviceClass = p[4];
    d->bDeviceSubClass = p[5];
    d->bDeviceProtocol = p[6];
    d->bMaxPacketSize0 = p[7];
    d->idVendor = le16(p + 8);
    d->idProduct = le16(p + 10);
    d->bcdDevice = le16(p + 12);
    d->iManufacturer = p[14];
    d->iProduct = p[15];
    d->iSerialNumber = p[16];
    d->bNumConfigurations = p[17];
    return 0;
}

/* Parses the configuration descriptor the way libusb does (one alt setting per interface). */
API int libusb_get_active_config_descriptor(libusb_device *dev, struct libusb_config_descriptor **out)
{
    const uint8_t *p = cfg_desc, *end = cfg_desc + cfg_len;
    struct libusb_config_descriptor *c = calloc(1, sizeof(*c));
    struct libusb_interface_descriptor *alt = NULL;
    int nif = 0;
    (void)dev;

    c->bLength = p[0];
    c->bDescriptorType = p[1];
    c->wTotalLength = le16(p + 2);
    c->bNumInterfaces = p[4];
    c->bConfigurationValue = p[5];
    c->iConfiguration = p[6];
    c->bmAttributes = p[7];
    c->MaxPower = p[8];
    struct libusb_interface *ifs = calloc(c->bNumInterfaces, sizeof(*ifs));
    c->interface = ifs;

    for (p += p[0]; p + 2 <= end && p[0] >= 2; p += p[0]) {
        if (p[1] == LIBUSB_DT_INTERFACE && nif < c->bNumInterfaces) {
            alt = calloc(1, sizeof(*alt));
            alt->bLength = p[0];
            alt->bDescriptorType = p[1];
            alt->bInterfaceNumber = p[2];
            alt->bAlternateSetting = p[3];
            alt->bNumEndpoints = p[4];
            alt->bInterfaceClass = p[5];
            alt->bInterfaceSubClass = p[6];
            alt->bInterfaceProtocol = p[7];
            alt->iInterface = p[8];
            alt->endpoint = calloc(alt->bNumEndpoints ? alt->bNumEndpoints : 1, sizeof(*alt->endpoint));
            alt->bNumEndpoints = 0;
            ifs[nif].altsetting = alt;
            ifs[nif].num_altsetting = 1;
            nif++;
        } else if (p[1] == LIBUSB_DT_ENDPOINT && alt) {
            struct libusb_endpoint_descriptor *e = (struct libusb_endpoint_descriptor *)&alt->endpoint[alt->bNumEndpoints++];
            e->bLength = p[0];
            e->bDescriptorType = p[1];
            e->bEndpointAddress = p[2];
            e->bmAttributes = p[3];
            e->wMaxPacketSize = le16(p + 4);
            e->bInterval = p[6];
        } else if (alt && alt->bNumEndpoints == 0) {
            /* class-specific descriptors between interface and endpoints */
            if (!alt->extra)
                alt->extra = p;
            alt->extra_length += p[0];
        }
    }
    *out = c;
    return 0;
}

API void libusb_free_config_descriptor(struct libusb_config_descriptor *c)
{
    if (!c)
        return;
    for (int i = 0; i < c->bNumInterfaces; i++) {
        if (c->interface[i].altsetting) {
            free((void *)c->interface[i].altsetting->endpoint);
            free((void *)c->interface[i].altsetting);
        }
    }
    free((void *)c->interface);
    free(c);
}

API int libusb_open(libusb_device *dev, libusb_device_handle **h)
{
    (void)dev;
    *h = &the_handle;
    return 0;
}

API void libusb_close(libusb_device_handle *h)
{
    (void)h;
}

API int libusb_claim_interface(libusb_device_handle *h, int n)
{
    (void)h;
    return n == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND;
}

API int libusb_release_interface(libusb_device_handle *h, int n)
{
    (void)h;
    (void)n;
    return 0;
}

API int libusb_set_configuration(libusb_device_handle *h, int config)
{
    (void)h;
    pthread_mutex_lock(&lock);
    int r = host_control(&host, 0x00, 9, (uint16_t)config, 0, NULL, 0);
    memset(host.toggle_in, 0, sizeof(host.toggle_in));
    memset(host.toggle_out, 0, sizeof(host.toggle_out));
    pthread_mutex_unlock(&lock);
    return r < 0 ? LIBUSB_ERROR_IO : 0;
}

static int map_error(int r)
{
    if (r >= 0)
        return r;
    if (r == MOCK_STALL)
        return LIBUSB_ERROR_PIPE;
    if (r == HOST_TIMEOUT)
        return LIBUSB_ERROR_TIMEOUT;
    return LIBUSB_ERROR_IO;
}

API int libusb_control_transfer(libusb_device_handle *h, uint8_t type, uint8_t req, uint16_t value,
                                uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout)
{
    (void)h;
    (void)timeout;
    pthread_mutex_lock(&lock);
    int r = host_control(&host, type, req, value, index, data, length);
    pthread_mutex_unlock(&lock);
    return map_error(r);
}

API int libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t idx, unsigned char *data, int length)
{
    uint8_t buf[255];
    int r, n = 0;

    (void)h;
    if (idx == 0)
        return LIBUSB_ERROR_INVALID_PARAM;
    pthread_mutex_lock(&lock);
    r = host_control(&host, 0x80, 6, (uint16_t)(0x0300 | idx), 0x0409, buf, sizeof(buf));
    pthread_mutex_unlock(&lock);
    if (r < 2)
        return map_error(r < 0 ? r : HOST_ERROR);
    for (int i = 2; i + 1 < r && n < length - 1; i += 2)
        data[n++] = buf[i + 1] ? '?' : buf[i];
    data[n] = 0;
    return n;
}

API int libusb_bulk_transfer(libusb_device_handle *h, unsigned char endpoint, unsigned char *data, int length,
                             int *transferred, unsigned int timeout)
{
    int r;
    (void)h;

    /* Bulk IN and OUT may run concurrently with control traffic: the
     * peripheral model serialises individual transactions. */
    if (endpoint & 0x80)
        r = host_bulk_in(&host, endpoint & 0x0f, data, (size_t)length, (int)timeout);
    else
        r = host_bulk_out(&host, endpoint & 0x0f, data, (size_t)length, (int)timeout);
    *transferred = r > 0 ? r : 0;
    return r < 0 ? map_error(r) : 0;
}

API int libusb_interrupt_transfer(libusb_device_handle *h, unsigned char endpoint, unsigned char *data,
                                  int length, int *transferred, unsigned int timeout)
{
    return libusb_bulk_transfer(h, endpoint, data, length, transferred, timeout);
}

API struct libusb_transfer *libusb_alloc_transfer(int iso_packets)
{
    return calloc(1, sizeof(struct libusb_transfer) +
                         (size_t)iso_packets * sizeof(struct libusb_iso_packet_descriptor));
}

API void libusb_free_transfer(struct libusb_transfer *t)
{
    free(t);
}

API int libusb_submit_transfer(struct libusb_transfer *t)
{
    pthread_mutex_lock(&lock);
    if (pending) {
        pthread_mutex_unlock(&lock);
        return LIBUSB_ERROR_BUSY;
    }
    pending = t;
    pending_cancelled = 0;
    pending_deadline = t->timeout ? now_ms() + t->timeout : 0;
    pthread_mutex_unlock(&lock);
    return 0;
}

API int libusb_cancel_transfer(struct libusb_transfer *t)
{
    pthread_mutex_lock(&lock);
    if (pending == t)
        pending_cancelled = 1;
    pthread_mutex_unlock(&lock);
    return 0;
}

API int libusb_handle_events_completed(libusb_context *ctx, int *completed)
{
    struct libusb_transfer *done = NULL;
    (void)ctx;

    if (completed && *completed)
        return 0;
    pthread_mutex_lock(&lock);
    if (pending) {
        struct libusb_transfer *t = pending;
        int toggle, r = MOCK_NAK;
        if (!pending_cancelled)
            r = mock_in(host.addr, t->endpoint & 0x0f, t->buffer, (unsigned)t->length, &toggle);
        if (r >= 0) {
            host.toggle_in[t->endpoint & 0x0f] ^= 1;
            t->actual_length = r;
            t->status = LIBUSB_TRANSFER_COMPLETED;
            done = t;
        } else if (pending_cancelled) {
            t->actual_length = 0;
            t->status = LIBUSB_TRANSFER_CANCELLED;
            done = t;
        } else if (r != MOCK_NAK) {
            t->actual_length = 0;
            t->status = r == MOCK_STALL ? LIBUSB_TRANSFER_STALL : LIBUSB_TRANSFER_ERROR;
            done = t;
        } else if (pending_deadline && now_ms() >= pending_deadline) {
            t->actual_length = 0;
            t->status = LIBUSB_TRANSFER_TIMED_OUT;
            done = t;
        }
        if (done)
            pending = NULL;
    }
    pthread_mutex_unlock(&lock);
    if (done)
        done->callback(done);
    else
        usleep(10000);
    return 0;
}
