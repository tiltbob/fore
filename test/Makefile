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

all: $(BUILD)/test_crypto $(BUILD)/test_usb $(BUILD)/test_piv $(BUILD)/test_preload $(BUILD)/test_preload_ca
	$(BUILD)/test_crypto
	$(BUILD)/test_piv
	$(BUILD)/test_usb
	$(BUILD)/test_preload
	$(BUILD)/test_preload_ca

e2e: all $(BUILD)/vpcd_card $(BUILD)/vpcd_card_novelty $(BUILD)/libfakeusb.so
	./e2e.sh vpcd
	./e2e.sh usb
	./e2e.sh novelty

$(BUILD)/test_crypto: test_crypto.c $(CRYPTO) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) -lcrypto

$(BUILD)/test_piv: test_piv.c $(APPLET) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) -lcrypto

# Factory-preloaded key (make PRELOAD_KEY=4), header from tools/gen-preload.py.
$(BUILD)/preload/preload.h: ../tools/gen-preload.py
	python3 ../tools/gen-preload.py --key 4 --out-dir $(BUILD)/preload

$(BUILD)/test_preload: test_preload.c $(APPLET) $(HEADERS) $(BUILD)/preload/preload.h
	$(CC) $(CFLAGS) -DFORE_PRELOAD -I$(BUILD)/preload -o $@ $(filter %.c,$^) -lcrypto

# The same with a supplied certificate (make PRELOAD_CERT=...): one issued by a
# throwaway P-256 CA. A certificate for a different key must be refused.
CA := $(BUILD)/preload-ca
$(CA)/preload.h: ../tools/gen-preload.py $(BUILD)/preload/preload.h
	@mkdir -p $(CA)
	openssl ecparam -name prime256v1 -genkey -noout -out $(CA)/ca.key
	openssl req -new -x509 -key $(CA)/ca.key -subj "/CN=fore test CA" -days 30 -out $(CA)/ca.pem
	openssl req -new -key $(BUILD)/preload/preload-key.pem -subj "/CN=four" -out $(CA)/four.csr
	openssl x509 -req -in $(CA)/four.csr -CA $(CA)/ca.pem -CAkey $(CA)/ca.key -set_serial 4 \
		-days 30 -outform DER -out $(CA)/four.der
	if python3 ../tools/gen-preload.py --key 5 --cert $(CA)/four.der --out-dir $(CA)/wrong; then \
		echo "certificate for another key was accepted"; exit 1; fi
	python3 ../tools/gen-preload.py --key 4 --cert $(CA)/four.der --out-dir $(CA)

$(BUILD)/test_preload_ca: test_preload.c $(APPLET) $(HEADERS) $(CA)/preload.h
	$(CC) $(CFLAGS) -DFORE_PRELOAD -I$(CA) -o $@ $(filter %.c,$^) -lcrypto

$(BUILD)/test_usb: test_usb.c $(USB) $(APPLET) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DUSB_HW_MOCK -o $@ $(filter %.c,$^) -lpthread

$(BUILD)/vpcd_card: ../host/vpcd_card.c $(APPLET) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -std=c99 -Wall -Wextra -D_DEFAULT_SOURCE -o $@ $(filter %.c,$^)

$(BUILD)/vpcd_card_novelty: ../host/vpcd_card.c $(APPLET) $(HEADERS) $(BUILD)/preload/preload.h
	$(CC) -O2 -g -std=c99 -Wall -Wextra -D_DEFAULT_SOURCE -DFORE_PRELOAD -I$(BUILD)/preload \
		-o $@ $(filter %.c,$^)

# Stand-in for libusb-1.0 that routes libccid's USB traffic into the firmware's
# USB driver running against the peripheral model (LD_PRELOAD it into pcscd).
$(BUILD)/libfakeusb.so: fake_libusb.c $(USB) $(APPLET) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -std=c99 -Wall -Wextra -D_DEFAULT_SOURCE -DUSB_HW_MOCK -fPIC -shared \
		-fvisibility=default -o $@ $(filter %.c,$^) -lpthread

clean:
	rm -rf $(BUILD)

.PHONY: all e2e clean
