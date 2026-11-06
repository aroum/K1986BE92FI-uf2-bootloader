/* Симулятор Flash: стирание -> 0xFF, программирование только 1->0 (AND). */
#include <string.h>
#include "sim_flash.h"
#include "bl_flash.h"

uint8_t  sim_flash[BL_FLASH_SIZE];
unsigned sim_erase_count[BL_FLASH_SIZE / BL_PAGE_SIZE];
unsigned sim_program_calls;
int      sim_fail_erase_page = -1;      /* номер страницы (от начала Flash), на которой стирание «ломается» */
int      sim_fail_program_after = -1;   /* после N успешных программирований — отказ */

void sim_reset(void)
{
    memset(sim_flash, 0xFF, sizeof sim_flash);
    memset(sim_flash, 0xA5, BL_CODE_SIZE);       /* «код загрузчика»: канарейка */
    memset(sim_erase_count, 0, sizeof sim_erase_count);
    sim_program_calls = 0;
    sim_fail_erase_page = -1;
    sim_fail_program_after = -1;
}

int sim_bootloader_intact(void)
{
    for (uint32_t i = 0; i < BL_CODE_SIZE; i++)
        if (sim_flash[i] != 0xA5) return 0;
    return 1;
}

const uint8_t *bl_flash_ptr(uint32_t addr)
{
    if (addr < BL_FLASH_BASE || addr >= BL_FLASH_END) return NULL;
    return &sim_flash[addr - BL_FLASH_BASE];
}

bool bl_flash_erase_page(uint32_t addr)
{
    if (!bl_flash_range_ok(addr, BL_PAGE_SIZE) || (addr & (BL_PAGE_SIZE - 1u))) return false;
    uint32_t page = (addr - BL_FLASH_BASE) / BL_PAGE_SIZE;
    if ((int)page == sim_fail_erase_page) return false;
    memset(&sim_flash[addr - BL_FLASH_BASE], 0xFF, BL_PAGE_SIZE);
    sim_erase_count[page]++;
    return true;
}

bool bl_flash_program(uint32_t addr, const void *data, uint32_t len)
{
    if (!bl_flash_range_ok(addr, len) || (addr & 3u) || (len & 3u)) return false;
    if (sim_fail_program_after >= 0 && (int)sim_program_calls >= sim_fail_program_after) return false;
    sim_program_calls++;
    const uint8_t *d = data;
    uint8_t *f = &sim_flash[addr - BL_FLASH_BASE];
    for (uint32_t i = 0; i < len; i++) f[i] &= d[i];
    return memcmp(f, d, len) == 0;               /* verify как на железе */
}
