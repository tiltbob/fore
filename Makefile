# Firmware for the ST-Link V2 (STM32F103C8). Host-side tests live in test/.
#
#   make                      build build/fore.elf, .bin and .hex
#   make USB_VID=0x.. USB_PID=0x..
#   make flash                program over SWD with st-flash
#   make test                 run the host test suites

CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size

BUILD := build
USB_VID ?= 0x1209
USB_PID ?= 0x0001

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

flash: $(BUILD)/fore.bin
	st-flash --reset write $< 0x08000000

test:
	$(MAKE) -C test

clean:
	rm -rf $(BUILD)

.PHONY: all flash test clean
