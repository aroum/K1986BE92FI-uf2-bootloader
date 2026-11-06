/* Абстракция Flash. Реализация: bl_flash.c (SPL, железо) / tests/sim_flash.c (хост). */
#ifndef BL_FLASH_H
#define BL_FLASH_H
#include <stdint.h>
#include <stdbool.h>
#include "bl_config.h"

/* Диапазон, доступный на запись/стирание: всё, что выше кода загрузчика
 * (страница состояния + приложение). Код загрузчика и Info-банк недоступны
 * ни при каких входных данных. */
static inline bool bl_flash_range_ok(uint32_t addr, uint32_t len)
{
    return addr >= BL_STATE_ADDR && addr < BL_FLASH_END &&
           len <= BL_FLASH_END - addr;
}

/* Указатель для чтения (Flash отображена в память). */
const uint8_t *bl_flash_ptr(uint32_t addr);

/* Стереть страницу (4 КБ). addr выровнен на страницу. Проверяет результат. */
bool bl_flash_erase_page(uint32_t addr);

/* Запрограммировать len байт (addr и len кратны 4). Область должна быть стёрта
 * (или менять только 1->0). Возвращает true только если чтение назад совпало. */
bool bl_flash_program(uint32_t addr, const void *data, uint32_t len);

#endif
