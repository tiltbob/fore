/*
 * STM32F103C8: 64 KiB flash (1 KiB pages), 20 KiB RAM. The top 14 KiB of
 * flash hold the PIV state, keys and certificates (see src/store.h) and are
 * not part of the firmware image, so reflashing keeps them.
 */
MEMORY
{
    FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 50K
    STORE (r)  : ORIGIN = 0x0800C800, LENGTH = 14K
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 20K
}

_store_start = ORIGIN(STORE);
_estack = ORIGIN(RAM) + LENGTH(RAM);
_min_stack = 6K;

ENTRY(reset_handler)

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx :
    {
        *(.ARM.exidx*)
    } > FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    ASSERT(_ebss + _min_stack <= _estack, "not enough RAM left for the stack")
}
