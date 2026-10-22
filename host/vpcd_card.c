/*
 * Runs the PIV applet as a virtual smart card behind vsmartcard's vpcd
 * reader driver, so pcscd, OpenSC, yubico-piv-tool and OpenSSL can talk to
 * it exactly as they would to the USB device.
 *
 *   vpcd_card [-p port] [-s flash-file] [-v]
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/piv.h"
#include "platform_host.h"

enum { CTRL_OFF = 0, CTRL_ON = 1, CTRL_RESET = 2, CTRL_ATR = 4 };

static int verbose;

static int read_full(int fd, uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t n = read(fd, buf, len);
        if (n <= 0)
            return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_msg(int fd, const uint8_t *buf, size_t len)
{
    uint8_t hdr[2] = {(uint8_t)(len >> 8), (uint8_t)len};
    if (write(fd, hdr, 2) != 2 || write(fd, buf, len) != (ssize_t)len)
        return -1;
    return 0;
}

static void dump(const char *dir, const uint8_t *p, size_t n)
{
    fprintf(stderr, "%s", dir);
    for (size_t i = 0; i < n; i++)
        fprintf(stderr, "%02X", p[i]);
    fprintf(stderr, "\n");
}

int main(int argc, char **argv)
{
    int port = 35963, opt, fd;
    const char *store = NULL;
    struct sockaddr_in sa;

    while ((opt = getopt(argc, argv, "p:s:u:v")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 's': store = optarg; break;
        case 'u': host_set_uid(optarg); break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "usage: %s [-p port] [-s flash-file] [-u uid] [-v]\n", argv[0]);
            return 2;
        }
    }

    host_flash_open(store);
    piv_init();

    fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect to vpcd");
        return 1;
    }

    for (;;) {
        uint8_t hdr[2], buf[65536], resp[PIV_MAX_RESPONSE];
        size_t len;

        if (read_full(fd, hdr, 2))
            break;
        len = (size_t)hdr[0] << 8 | hdr[1];
        if (read_full(fd, buf, len))
            break;
        if (len == 1) {
            switch (buf[0]) {
            case CTRL_OFF:
            case CTRL_ON:
            case CTRL_RESET:
                piv_reset();
                break;
            case CTRL_ATR:
                send_msg(fd, piv_atr, piv_atr_len);
                break;
            }
            continue;
        }
        if (verbose)
            dump("> ", buf, len);
        size_t n = piv_apdu(buf, len, resp);
        if (verbose)
            dump("< ", resp, n);
        if (send_msg(fd, resp, n))
            break;
    }
    return 0;
}
