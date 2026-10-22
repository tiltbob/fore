/*
 * fore: PIV smart card firmware for ST-Link V2 dongles (STM32F103C8, 8 MHz
 * crystal, LED on PA9, USB D+ with a fixed pull-up on PA12).
 */
#include <stdint.h>

#include "../src/ccid.h"
#include "../src/piv.h"
#include "../src/platform.h"
#include "../src/sha512.h"
#include "stm32f103.h"
#include "usb.h"

#define LED_PIN 9
#define USB_DP_PIN 12

static volatile uint32_t ticks;

void systick_handler(void)
{
    ticks++;
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = ticks;
    while (ticks - start < ms)
        ;
}

static void clock_init(void)
{
    RCC_CR |= RCC_CR_HSEON;
    while (!(RCC_CR & RCC_CR_HSERDY))
        ;
    FLASH_ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY2;
    /* 8 MHz x 9 = 72 MHz; APB1 36 MHz; ADC 12 MHz; USB 72 / 1.5 = 48 MHz */
    RCC_CFGR = RCC_CFGR_PLLMUL9 | RCC_CFGR_PLLSRC_HSE | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_ADCPRE_DIV6;
    RCC_CR |= RCC_CR_PLLON;
    while (!(RCC_CR & RCC_CR_PLLRDY))
        ;
    RCC_CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC_CFGR & RCC_CFGR_SWS_MASK) != RCC_CFGR_SWS_PLL)
        ;

    SYST_RVR = 72000 - 1;
    SYST_CVR = 0;
    SYST_CSR = 7; /* core clock, interrupt, enable */

    DEMCR |= 1u << 24; /* TRCENA, for the cycle counter */
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1;
}

/* Sets the 4-bit mode/config field of a GPIOA pin in 8..15. */
static void gpioa_mode(unsigned pin, uint32_t mode)
{
    unsigned shift = 4 * (pin - 8);
    GPIOA_CRH = (GPIOA_CRH & ~(0xfu << shift)) | (mode << shift);
}

void platform_activity(int on)
{
    if (on)
        GPIOA_BSRR = 1u << LED_PIN;
    else
        GPIOA_BRR = 1u << LED_PIN;
}

/* ---- platform hooks ---- */

extern uint8_t _store_start[];

const uint8_t *flash_page(unsigned page)
{
    return _store_start + page * FLASH_PAGE_SIZE;
}

static int flash_wait(void)
{
    uint32_t sr;
    while ((sr = FLASH_SR) & FLASH_SR_BSY)
        ;
    FLASH_SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;
    return (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) ? -1 : 0;
}

static void flash_unlock(void)
{
    if (FLASH_CR & FLASH_CR_LOCK) {
        FLASH_KEYR = 0x45670123;
        FLASH_KEYR = 0xcdef89ab;
    }
}

int flash_erase(unsigned page)
{
    int r;

    if (page >= STORE_PAGES)
        return -1;
    flash_unlock();
    flash_wait();
    FLASH_CR = FLASH_CR_PER;
    FLASH_AR = (uint32_t)flash_page(page);
    FLASH_CR = FLASH_CR_PER | FLASH_CR_STRT;
    r = flash_wait();
    FLASH_CR = FLASH_CR_LOCK;
    return r;
}

int flash_program(unsigned page, unsigned off, const void *src, size_t len)
{
    volatile uint16_t *dst = (volatile uint16_t *)(flash_page(page) + off);
    const uint8_t *s = src;
    int r = 0;

    if ((off | len) & 1 || page * FLASH_PAGE_SIZE + off + len > STORE_PAGES * FLASH_PAGE_SIZE)
        return -1;
    flash_unlock();
    flash_wait();
    FLASH_CR = FLASH_CR_PG;
    for (size_t i = 0; i < len && r == 0; i += 2) {
        *dst++ = (uint16_t)(s[i] | s[i + 1] << 8);
        r = flash_wait();
    }
    FLASH_CR = FLASH_CR_LOCK;
    return r;
}

const uint8_t *device_uid(void)
{
    return (const uint8_t *)UID_BASE;
}

/*
 * Randomness for management key challenges. There is no TRNG on the F103,
 * so the pool is seeded from ADC noise on the temperature sensor and
 * reference, plus cycle-counter jitter, and every draw mixes in the cycle
 * counter (USB traffic timing makes that vary).
 */
static uint8_t pool[64];

static void pool_mix(const void *data, size_t len, uint8_t label)
{
    sha512_ctx c;
    uint32_t t = DWT_CYCCNT;

    sha512_init(&c);
    sha512_update(&c, &label, 1);
    sha512_update(&c, pool, sizeof(pool));
    sha512_update(&c, &t, sizeof(t));
    sha512_update(&c, data, len);
    sha512_final(&c, pool, sizeof(pool));
}

static void rng_init(void)
{
    uint16_t samples[256];

    RCC_APB2ENR |= RCC_APB2ENR_ADC1EN;
    ADC1_CR2 = ADC_CR2_ADON | ADC_CR2_TSVREFE;
    delay_ms(1);
    ADC1_CR2 |= ADC_CR2_CAL;
    while (ADC1_CR2 & ADC_CR2_CAL)
        ;
    ADC1_SMPR1 = 0; /* shortest sample time: noisiest */
    ADC1_CR2 |= ADC_CR2_EXTTRIG | ADC_CR2_EXTSEL_SWSTART;
    for (unsigned round = 0; round < 8; round++) {
        for (unsigned i = 0; i < 256; i++) {
            ADC1_SQR3 = 16 + (i & 1); /* temperature sensor, Vrefint */
            ADC1_CR2 |= ADC_CR2_SWSTART;
            while (!(ADC1_SR & ADC_SR_EOC))
                ;
            samples[i] = (uint16_t)(ADC1_DR ^ (DWT_CYCCNT << 12));
        }
        pool_mix(samples, sizeof(samples), 's');
    }
    pool_mix(device_uid(), 12, 'u');
    ADC1_CR2 = 0;
    RCC_APB2ENR &= ~RCC_APB2ENR_ADC1EN;
}

void rng_fill(uint8_t *buf, size_t len)
{
    while (len) {
        uint8_t block[64];
        size_t n = len < 32 ? len : 32;
        sha512_ctx c;
        uint32_t t = DWT_CYCCNT;

        sha512_init(&c);
        sha512_update(&c, "out", 3);
        sha512_update(&c, pool, sizeof(pool));
        sha512_update(&c, &t, sizeof(t));
        sha512_final(&c, block, sizeof(block));
        for (size_t i = 0; i < n; i++)
            buf[i] = block[i];
        buf += n;
        len -= n;
        pool_mix(block + 32, 32, 'k'); /* ratchet forward */
    }
}

int main(void)
{
    clock_init();

    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN;
    gpioa_mode(LED_PIN, 0x2); /* push-pull output, 2 MHz */
    platform_activity(1);

    /* The dongle has a fixed pull-up on D+, so hold D+ low for a moment to
     * make the host notice a fresh attach after a reset or reflash. */
    GPIOA_BRR = 1u << USB_DP_PIN;
    gpioa_mode(USB_DP_PIN, 0x2);
    rng_init();
    delay_ms(20);
    gpioa_mode(USB_DP_PIN, 0x4); /* floating input; the USB peripheral takes over */

    piv_init();
    RCC_APB1ENR |= RCC_APB1ENR_USBEN;
    usb_init();
    NVIC_ISER0 = 1u << USB_LP_IRQ;
    platform_activity(0);

    for (;;) {
        ccid_poll();
        __asm__ volatile("wfi");
    }
}
