# Host-side tests. Needs a native C compiler and OpenSSL's libcrypto.
#
#   make          crypto, usb and applet unit tests
#   make e2e      also run the pcscd/OpenSC/yubico-piv-tool/OpenSSL tests
#                 (needs pcscd, libccid, vsmartcard-vpcd, opensc,
#                 yubico-piv-tool and libengine-pkcs11-openssl/pkcs11-provider)

CC ?= cc
BUILD := ../build/test
CFLAGS := -O1 -g -std=c99 -Wall -Wextra -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=all
CRYPTO := ../src/p384.c ../src/sha512.c ../src/des.c
APPLET := ../src/piv.c ../src/store.c ../host/platform_host.c $(CRYPTO)
USB := ../stm32/usb.c ../src/ccid.c usb_mock.c
HEADERS := $(wildcard ../src/*.h ../stm32/*.h ../host/*.h *.h)

all: $(BUILD)/test_crypto $(BUILD)/test_usb $(BUILD)/test_piv
	$(BUILD)/test_crypto
	$(BUILD)/test_piv
	$(BUILD)/test_usb

e2e: all $(BUILD)/vpcd_card $(BUILD)/libfakeusb.so
	./e2e.sh vpcd
	./e2e.sh usb

$(BUILD)/test_crypto: test_crypto.c $(CRYPTO) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) -lcrypto

$(BUILD)/test_piv: test_piv.c $(APPLET) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) -lcrypto

$(BUILD)/test_usb: test_usb.c $(USB) $(APPLET) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DUSB_HW_MOCK -o $@ $(filter %.c,$^) -lpthread

$(BUILD)/vpcd_card: ../host/vpcd_card.c $(APPLET) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -std=c99 -Wall -Wextra -D_DEFAULT_SOURCE -o $@ $(filter %.c,$^)

# Stand-in for libusb-1.0 that routes libccid's USB traffic into the firmware's
# USB driver running against the peripheral model (LD_PRELOAD it into pcscd).
$(BUILD)/libfakeusb.so: fake_libusb.c $(USB) $(APPLET) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -std=c99 -Wall -Wextra -D_DEFAULT_SOURCE -DUSB_HW_MOCK -fPIC -shared \
		-fvisibility=default -o $@ $(filter %.c,$^) -lpthread

clean:
	rm -rf $(BUILD)

.PHONY: all e2e clean
