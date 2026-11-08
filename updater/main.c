#include "MDR32FxQI_config.h"
#include "MDR32FxQI_rst_clk.h"
#include "MDR32FxQI_eeprom.h"
#include "MDR32FxQI_port.h"
#include "bl_config.h"
#include <stdint.h>
#include <stdbool.h>

extern const uint8_t bootloader_bin[];
extern const uint32_t bootloader_bin_size;

#define PAGE_SIZE 4096u
#define BOOTLOADER_MAX_SIZE 12288u /* 12 KB (3 pages of 4 KB) */

/* Staging buffer in RAM so source data is 100% in RAM before we erase Flash! */
static uint8_t ram_image_buffer[BOOTLOADER_MAX_SIZE];

static void led_init(void) {
    RST_CLK_PCLKcmd(RST_CLK_PCLK_PORTB, ENABLE);
    PORT_InitTypeDef conf;
    PORT_StructInit(&conf);
    conf.PORT_Pin = PORT_Pin_7;
    conf.PORT_OE = PORT_OE_OUT;
    conf.PORT_FUNC = PORT_FUNC_PORT;
    conf.PORT_MODE = PORT_MODE_DIGITAL;
    conf.PORT_SPEED = PORT_SPEED_MAXFAST;
    PORT_Init(MDR_PORTB, &conf);
    PORT_ResetBits(MDR_PORTB, PORT_Pin_7);
}

__RAMFUNC __attribute__((noinline)) static void led_set_ram(bool on) {
    if (on) {
        MDR_PORTB->RXTX |= (1u << 7);
    } else {
        MDR_PORTB->RXTX &= ~(1u << 7);
    }
}

__RAMFUNC __attribute__((noinline)) static void delay_ram(volatile uint32_t count) {
    while (count--) {
        __NOP();
    }
}

__RAMFUNC __attribute__((noinline)) static void ram_erase_page(uint32_t addr) {
    EEPROM_ErasePage(addr, EEPROM_Main_Bank_Select);
    EEPROM_UpdateDCache();
}

__RAMFUNC __attribute__((noinline)) static void ram_program_word(uint32_t addr, uint32_t val) {
    EEPROM_ProgramWord(addr, EEPROM_Main_Bank_Select, val);
    EEPROM_UpdateDCache();
}

/* Entire update routine executed strictly from RAM with interrupts disabled.
 * Both code and data are 100% in RAM. */
__RAMFUNC __attribute__((noinline, noreturn)) static void perform_update_and_reboot(const uint8_t *src, uint32_t size) {
    __disable_irq();

    if (size == 0 || size > BOOTLOADER_MAX_SIZE) {
        goto failure;
    }

    uint32_t pages = (size + PAGE_SIZE - 1u) / PAGE_SIZE;

    /* 1. Erase and write bootloader pages (0x08000000, 0x08001000, ...) */
    for (uint32_t p = 0; p < pages; p++) {
        uint32_t page_addr = BL_FLASH_BASE + p * PAGE_SIZE;
        ram_erase_page(page_addr);

        /* Verify page erase */
        const volatile uint32_t *dst32 = (const volatile uint32_t *)(uintptr_t)page_addr;
        for (uint32_t i = 0; i < PAGE_SIZE / 4u; i++) {
            if (dst32[i] != 0xFFFFFFFFu) {
                goto failure;
            }
        }

        /* Write data word-by-word directly from RAM buffer */
        uint32_t page_bytes = size - (p * PAGE_SIZE);
        if (page_bytes > PAGE_SIZE) {
            page_bytes = PAGE_SIZE;
        }

        const uint8_t *page_src = src + (p * PAGE_SIZE);
        for (uint32_t off = 0; off < page_bytes; off += 4u) {
            uint32_t word = 0xFFFFFFFFu;
            uint32_t chunk = page_bytes - off;
            if (chunk >= 4u) {
                word = (uint32_t)page_src[off] |
                       ((uint32_t)page_src[off + 1] << 8) |
                       ((uint32_t)page_src[off + 2] << 16) |
                       ((uint32_t)page_src[off + 3] << 24);
            } else {
                for (uint32_t b = 0; b < chunk; b++) {
                    word &= ~(0xFFu << (b * 8));
                    word |= ((uint32_t)page_src[off + b] << (b * 8));
                }
            }
            ram_program_word(page_addr + off, word);
        }

        /* Verify written data against RAM source */
        for (uint32_t off = 0; off < page_bytes; off += 4u) {
            uint32_t expected = 0xFFFFFFFFu;
            uint32_t chunk = page_bytes - off;
            if (chunk >= 4u) {
                expected = (uint32_t)page_src[off] |
                           ((uint32_t)page_src[off + 1] << 8) |
                           ((uint32_t)page_src[off + 2] << 16) |
                           ((uint32_t)page_src[off + 3] << 24);
            } else {
                for (uint32_t b = 0; b < chunk; b++) {
                    expected &= ~(0xFFu << (b * 8));
                    expected |= ((uint32_t)page_src[off + b] << (b * 8));
                }
            }
            uint32_t actual = *(volatile uint32_t *)(uintptr_t)(page_addr + off);
            if (actual != expected) {
                goto failure;
            }
        }
    }

    /* 2. Invalidate app state page so bootloader stays in UF2 or does clean validation */
    ram_erase_page(BL_STATE_ADDR);

    /* 3. Invalidate updater application itself (erase first page of app 0x08004000)
     * so it doesn't boot back into updater loop */
    ram_erase_page(BL_APP_START);

    /* Success: LED solid ON for ~1 second, then reboot into updated bootloader */
    led_set_ram(true);
    delay_ram(1000000);
    NVIC_SystemReset();
    while (1) { }

failure:
    /* Error: rapid blinking forever */
    while (1) {
        led_set_ram(true);
        delay_ram(80000);
        led_set_ram(false);
        delay_ram(80000);
    }
}

int main(void) {
    /* Set VTOR */
    SCB->VTOR = BL_APP_START;

    led_init();

    /* Check bootloader image size */
    if (bootloader_bin_size == 0 || bootloader_bin_size > BOOTLOADER_MAX_SIZE) {
        while (1) {
            led_set_ram(true);
            delay_ram(80000);
            led_set_ram(false);
            delay_ram(80000);
        }
    }

    /* Copy entire new bootloader image into RAM staging buffer */
    for (uint32_t i = 0; i < bootloader_bin_size; i++) {
        ram_image_buffer[i] = bootloader_bin[i];
    }

    /* Blink fast 5 times to indicate update is about to execute */
    for (int i = 0; i < 5; i++) {
        led_set_ram(true);
        delay_ram(200000);
        led_set_ram(false);
        delay_ram(200000);
    }

    /* Execute update from RAM buffer */
    perform_update_and_reboot(ram_image_buffer, bootloader_bin_size);

    return 0;
}
