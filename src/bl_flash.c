/* Flash на SPL. Код стирания/программирования исполняется из ОЗУ (__RAMFUNC в SPL),
 * на время операций прерывания запрещены (ISR лежат во Flash). */
#include <string.h>
#include "MDR32FxQI_config.h"
#include "MDR32FxQI_eeprom.h"
#include "bl_flash.h"

const uint8_t *bl_flash_ptr(uint32_t addr) { return (const uint8_t *)(uintptr_t)addr; }

__RAMFUNC __attribute__((noinline)) static void ram_erase_page(uint32_t addr)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    EEPROM_ErasePage(addr, EEPROM_Main_Bank_Select);
    EEPROM_UpdateDCache();
    __set_PRIMASK(primask);
}

__RAMFUNC __attribute__((noinline)) static void ram_program_word(uint32_t addr, uint32_t val)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    EEPROM_ProgramWord(addr, EEPROM_Main_Bank_Select, val);
    EEPROM_UpdateDCache();
    __set_PRIMASK(primask);
}

bool bl_flash_erase_page(uint32_t addr)
{
    if (!bl_flash_range_ok(addr, BL_PAGE_SIZE) || (addr & (BL_PAGE_SIZE - 1u))) return false;

    ram_erase_page(addr);

    const uint32_t *p = (const uint32_t *)(uintptr_t)addr;
    for (uint32_t i = 0; i < BL_PAGE_SIZE / 4u; i++)
        if (p[i] != 0xFFFFFFFFu) return false;
    return true;
}

bool bl_flash_program(uint32_t addr, const void *data, uint32_t len)
{
    if (!bl_flash_range_ok(addr, len) || (addr & 3u) || (len & 3u)) return false;

    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t off = 0; off < len; off += 4u) {
        uint32_t w, cur;
        memcpy(&w, src + off, 4);
        memcpy(&cur, (const void *)(uintptr_t)(addr + off), 4);
        if (cur == w) continue;                   /* уже записано / стёрто и 0xFFFFFFFF */

        ram_program_word(addr + off, w);
    }
    return memcmp((const void *)(uintptr_t)addr, data, len) == 0;
}
