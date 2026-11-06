/* Запрос входа в UF2-режим из приложения (и проверка в загрузчике).
 * 64-битное «магическое» значение в последних 8 байтах ОЗУ переживает программный
 * сброс. Линкер-скрипт приложения ДОЛЖЕН оставлять последние 16 байт ОЗУ свободными:
 *   _estack = ORIGIN(RAM) + LENGTH(RAM) - 16;
 * Использование в приложении:
 *   #include "boot_request.h"
 *   bl_request_uf2_and_reset();
 */
#ifndef BOOT_REQUEST_H
#define BOOT_REQUEST_H
#include <stdint.h>

#define BL_REQUEST_ADDR   0x20007FF8u
#define BL_REQUEST_MAGIC  0xB007F2D0AD15C0DEull

static inline void bl_request_uf2(void)
{
    *(volatile uint64_t *)BL_REQUEST_ADDR = BL_REQUEST_MAGIC;
}

/* Прочитать и сразу погасить запрос (чтобы он не «залипал» между сбросами). */
static inline int bl_take_request(void)
{
    volatile uint64_t *p = (volatile uint64_t *)BL_REQUEST_ADDR;
    int req = (*p == BL_REQUEST_MAGIC);
    *p = 0;
    return req;
}

#ifdef __ARM_ARCH
#include "K1986VE9xI.h"          /* NVIC_SystemReset через CMSIS (из SPL) */
static inline void bl_request_uf2_and_reset(void)
{
    bl_request_uf2();
    NVIC_SystemReset();
}
#endif
#endif
