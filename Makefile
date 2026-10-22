# Firmware for the ST-Link V2 (STM32F103C8). Host-side tests live in test/.
#
#   make                      build build/fore.elf, .bin and .hex
#   make USB_VID=0x.. USB_PID=0x..
#   make PRELOAD_KEY=<hex> [PRELOAD_SLOT=9a] [PRELOAD_CERT=<pem|der>] BUILD=<dir>
#                             factory-default key and certificate (generated
#                             self-signed unless PRELOAD_CERT is given)
#   make novelty              the above with d = 4, in build/novelty
#   make flash                program over SWD with st-flash
#   make test                 run the host test suites

CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size

BUILD := build
USB_VID ?= 0x1209
USB_PID ?= 0x0001
PRELOAD_KEY ?=
PRELOAD_SLOT ?= 9a
PRELOAD_CERT ?=

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

ifneq ($(PRELOAD_CERT),)
ifeq ($(PRELOAD_KEY)$(filter novelty,$(MAKECMDGOALS)),)
$(error PRELOAD_CERT needs PRELOAD_KEY, the private key it belongs to)
endif
endif

ifneq ($(PRELOAD_KEY),)
# Keep preloaded objects away from the normal build so it never inherits the key.
ifeq ($(BUILD),build)
$(error PRELOAD_KEY needs its own BUILD=<dir> (or use make novelty))
endif
CFLAGS += -DFORE_PRELOAD -I$(BUILD)
$(BUILD)/fw/src/store.o: $(BUILD)/preload.h
$(BUILD)/preload.h: tools/gen-preload.py $(BUILD)/preload.id $(PRELOAD_CERT)
	python3 tools/gen-preload.py --key $(PRELOAD_KEY) --slot $(PRELOAD_SLOT) \
		$(if $(PRELOAD_CERT),--cert $(PRELOAD_CERT)) --out-dir $(BUILD)
# Regenerate when the key, slot or certificate choice changes.
PRELOAD_ID := $(PRELOAD_KEY) $(PRELOAD_SLOT) $(PRELOAD_CERT)
$(BUILD)/preload.id: FORCE
	@mkdir -p $(BUILD)
	@echo '$(PRELOAD_ID)' | cmp -s - $@ || echo '$(PRELOAD_ID)' > $@
endif

# A deliberately insecure novelty build: the private key is 4.
novelty:
	$(MAKE) BUILD=$(BUILD)/novelty PRELOAD_KEY=4 PRELOAD_SLOT=$(PRELOAD_SLOT)

flash: $(BUILD)/fore.bin
	st-flash --reset write $< 0x08000000

test:
	$(MAKE) -C test

clean:
	rm -rf $(BUILD)

.PHONY: all novelty flash test clean FORCE
