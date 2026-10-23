# Firmware for the ST-Link V2 (STM32F103C8). Host-side tests live in test/.
#
#   make                      build build/fore.elf, .bin and .hex
#   make USB_VID=0x.. USB_PID=0x..
#   make PRELOAD_KEY=<hex|key file> [PRELOAD_SLOT=9a] [PRELOAD_CERT=<pem|der>] BUILD=<dir>
#                             factory-default key and certificate (generated
#                             self-signed unless PRELOAD_CERT is given)
#   make ... PRELOAD_LOCKED=1   locked: only the preloaded key exists, and it
#                             cannot be replaced or deleted over the card interface
#   make ... PRELOAD_PIN=<6-8 digits>   factory PIN instead of 123456
#   make novelty              the above with d = 4 and PIN 8675309, in build/novelty
#   make flash                program over SWD with st-flash
#   make test                 run the host test suites

CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size

BUILD := build
USB_VID ?= 0x15e1
USB_PID ?= 0x2007
PRELOAD_KEY ?=
PRELOAD_SLOT ?= 9a
PRELOAD_CERT ?=
PRELOAD_CN ?=
PRELOAD_LOCKED ?= 0
PRELOAD_PIN ?=

SRCS := stm32/startup.c stm32/main.c stm32/usb.c \
        src/ccid.c src/piv.c src/store.c src/p384.c src/sha512.c src/des.c
OBJS := $(SRCS:%.c=$(BUILD)/fw/%.o)

CFLAGS := -mcpu=cortex-m3 -mthumb -Os -g -std=c99 -Wall -Wextra \
          -ffunction-sections -fdata-sections -ffreestanding \
          -DUSB_VID=$(USB_VID) -DUSB_PID=$(USB_PID) -MMD -MP
LDFLAGS := -mcpu=cortex-m3 -mthumb -nostartfiles --specs=nano.specs --specs=nosys.specs \
           -Wl,--gc-sections -Wl,-Tstm32/stm32f103c8.ld -Wl,-Map=$(BUILD)/fore.map

all: $(BUILD)/fore.bin $(BUILD)/fore.hex

$(BUILD)/fw/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fore.elf: $(OBJS) stm32/stm32f103c8.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@
	$(SIZE) $@

$(BUILD)/fore.bin: $(BUILD)/fore.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/fore.hex: $(BUILD)/fore.elf
	$(OBJCOPY) -O ihex $< $@

-include $(OBJS:.o=.d)

# Rebuild the USB descriptors when the USB ID changes.
$(BUILD)/fw/stm32/usb.o: $(BUILD)/usb.id
$(BUILD)/usb.id: FORCE
	@mkdir -p $(BUILD)
	@echo '$(USB_VID):$(USB_PID)' | cmp -s - $@ || echo '$(USB_VID):$(USB_PID)' > $@

ifneq ($(PRELOAD_CERT),)
ifeq ($(PRELOAD_KEY)$(filter novelty,$(MAKECMDGOALS)),)
$(error PRELOAD_CERT needs PRELOAD_KEY, the private key it belongs to)
endif
endif
ifneq ($(PRELOAD_PIN),)
ifeq ($(PRELOAD_KEY)$(filter novelty,$(MAKECMDGOALS)),)
$(error PRELOAD_PIN needs PRELOAD_KEY (or make novelty))
endif
endif
ifneq ($(filter-out 0 1,$(PRELOAD_LOCKED)),)
$(error PRELOAD_LOCKED must be 0 or 1)
endif
ifeq ($(PRELOAD_LOCKED),1)
ifeq ($(PRELOAD_KEY)$(filter novelty,$(MAKECMDGOALS)),)
$(error PRELOAD_LOCKED needs PRELOAD_KEY, the key to lock in)
endif
endif

ifneq ($(PRELOAD_KEY),)
# Keep preloaded objects away from the normal build so it never inherits the key.
ifeq ($(BUILD),build)
$(error PRELOAD_KEY needs its own BUILD=<dir> (or use make novelty))
endif
CFLAGS += -DFORE_PRELOAD $(if $(filter 1,$(PRELOAD_LOCKED)),-DFORE_LOCKED) -I$(BUILD)
$(BUILD)/fw/src/store.o $(BUILD)/fw/src/piv.o: $(BUILD)/preload.h
# PRELOAD_KEY is a hex scalar or a key file; a file is also a prerequisite.
$(BUILD)/preload.h: tools/gen-preload.py $(BUILD)/preload.id $(wildcard $(PRELOAD_KEY)) $(PRELOAD_CERT)
	python3 tools/gen-preload.py --key $(PRELOAD_KEY) --slot $(PRELOAD_SLOT) \
		$(if $(PRELOAD_CERT),--cert $(PRELOAD_CERT)) $(if $(PRELOAD_CN),--cn '$(PRELOAD_CN)') \
		$(if $(PRELOAD_PIN),--pin '$(PRELOAD_PIN)') --out-dir $(BUILD)
# Regenerate (and recompile its users) when any preload setting changes.
PRELOAD_ID := $(PRELOAD_KEY) $(PRELOAD_SLOT) $(PRELOAD_CERT) $(PRELOAD_CN) $(PRELOAD_LOCKED) $(PRELOAD_PIN)
$(BUILD)/preload.id: FORCE
	@mkdir -p $(BUILD)
	@echo '$(PRELOAD_ID)' | cmp -s - $@ || echo '$(PRELOAD_ID)' > $@
endif

# A deliberately insecure novelty build: the private key is 4, the PIN 8675309.
novelty:
	$(MAKE) BUILD=$(BUILD)/novelty PRELOAD_KEY=4 PRELOAD_SLOT=$(PRELOAD_SLOT) \
		PRELOAD_PIN=$(or $(PRELOAD_PIN),8675309) 'PRELOAD_CN=fore insecure key (d = 0x4)'

flash: $(BUILD)/fore.bin
	st-flash --reset write $< 0x08000000

test:
	$(MAKE) -C test

clean:
	rm -rf $(BUILD)

.PHONY: all novelty flash test clean FORCE
