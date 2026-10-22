/* The handful of STM32F103 / Cortex-M3 registers this firmware touches. */
#ifndef STM32F103_H
#define STM32F103_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

#define RCC_BASE 0x40021000u
#define RCC_CR REG32(RCC_BASE + 0x00)
#define RCC_CFGR REG32(RCC_BASE + 0x04)
#define RCC_APB2ENR REG32(RCC_BASE + 0x18)
#define RCC_APB1ENR REG32(RCC_BASE + 0x1c)

#define RCC_CR_HSEON (1u << 16)
#define RCC_CR_HSERDY (1u << 17)
#define RCC_CR_PLLON (1u << 24)
#define RCC_CR_PLLRDY (1u << 25)
#define RCC_CFGR_SW_PLL (2u << 0)
#define RCC_CFGR_SWS_MASK (3u << 2)
#define RCC_CFGR_SWS_PLL (2u << 2)
#define RCC_CFGR_PPRE1_DIV2 (4u << 8)
#define RCC_CFGR_ADCPRE_DIV6 (2u << 14)
#define RCC_CFGR_PLLSRC_HSE (1u << 16)
#define RCC_CFGR_PLLMUL9 (7u << 18)
#define RCC_APB2ENR_IOPAEN (1u << 2)
#define RCC_APB2ENR_ADC1EN (1u << 9)
#define RCC_APB1ENR_USBEN (1u << 23)

#define FLASH_R_BASE 0x40022000u
#define FLASH_ACR REG32(FLASH_R_BASE + 0x00)
#define FLASH_KEYR REG32(FLASH_R_BASE + 0x04)
#define FLASH_SR REG32(FLASH_R_BASE + 0x0c)
#define FLASH_CR REG32(FLASH_R_BASE + 0x10)
#define FLASH_AR REG32(FLASH_R_BASE + 0x14)

#define FLASH_ACR_LATENCY2 2u
#define FLASH_ACR_PRFTBE (1u << 4)
#define FLASH_SR_BSY (1u << 0)
#define FLASH_SR_PGERR (1u << 2)
#define FLASH_SR_WRPRTERR (1u << 4)
#define FLASH_SR_EOP (1u << 5)
#define FLASH_CR_PG (1u << 0)
#define FLASH_CR_PER (1u << 1)
#define FLASH_CR_STRT (1u << 6)
#define FLASH_CR_LOCK (1u << 7)

#define GPIOA_BASE 0x40010800u
#define GPIOA_CRH REG32(GPIOA_BASE + 0x04)
#define GPIOA_BSRR REG32(GPIOA_BASE + 0x10)
#define GPIOA_BRR REG32(GPIOA_BASE + 0x14)

#define ADC1_BASE 0x40012400u
#define ADC1_SR REG32(ADC1_BASE + 0x00)
#define ADC1_CR2 REG32(ADC1_BASE + 0x08)
#define ADC1_SMPR1 REG32(ADC1_BASE + 0x0c)
#define ADC1_SQR3 REG32(ADC1_BASE + 0x34)
#define ADC1_DR REG32(ADC1_BASE + 0x4c)

#define ADC_SR_EOC (1u << 1)
#define ADC_CR2_ADON (1u << 0)
#define ADC_CR2_CAL (1u << 2)
#define ADC_CR2_RSTCAL (1u << 3)
#define ADC_CR2_EXTSEL_SWSTART (7u << 17)
#define ADC_CR2_EXTTRIG (1u << 20)
#define ADC_CR2_SWSTART (1u << 22)
#define ADC_CR2_TSVREFE (1u << 23)

#define USB_BASE 0x40005c00u
#define USB_PMA_BASE 0x40006000u

#define SYST_CSR REG32(0xe000e010u)
#define SYST_RVR REG32(0xe000e014u)
#define SYST_CVR REG32(0xe000e018u)
#define NVIC_ISER0 REG32(0xe000e100u)
#define SCB_AIRCR REG32(0xe000ed0cu)
#define DEMCR REG32(0xe000edfcu)
#define DWT_CTRL REG32(0xe0001000u)
#define DWT_CYCCNT REG32(0xe0001004u)

#define USB_LP_IRQ 20

#define UID_BASE 0x1ffff7e8u

#endif
