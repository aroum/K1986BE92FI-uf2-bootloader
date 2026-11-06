#include <string.h>
#include "app_state.h"
#include "bl_flash.h"

/* Слова страницы состояния:
 *   w0: 0xFFFFFFFF  стёрто (приложение залито не нашим загрузчиком, например по SWD
 *                   -> достаточно проверки векторов)
 *       ST_VALID    обновление завершено и проверено (w1..w3 валидны)
 *       0x00000000  обновление идёт/не завершено -> загрузка запрещена
 * Коммит: стереть страницу, записать w1..w3, и только затем w0. */
#define ST_ERASED 0xFFFFFFFFu
#define ST_VALID  0xB007C0DEu

typedef struct { uint32_t state, app_end, crc, check; } state_t;

static uint32_t chk(const state_t *s)
{
    return s->state ^ s->app_end ^ s->crc ^ 0xA5A5A5A5u;
}

uint32_t app_crc32(uint32_t start, uint32_t len)
{
    const uint8_t *p = bl_flash_ptr(start);
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool app_vectors_sane(void)
{
    uint32_t sp, pc;
    memcpy(&sp, bl_flash_ptr(BL_APP_START), 4);
    memcpy(&pc, bl_flash_ptr(BL_APP_START + 4), 4);
    if (sp <= BL_RAM_BASE || sp > BL_RAM_BASE + BL_RAM_SIZE || (sp & 7u)) return false;
    if (!(pc & 1u)) return false;                       /* Thumb */
    pc &= ~1u;
    return pc >= BL_APP_START && pc < BL_FLASH_END;
}

bool app_state_is_bootable(void)
{
    state_t s;
    memcpy(&s, bl_flash_ptr(BL_STATE_ADDR), sizeof s);
    if (s.state == 0u) return false;
    if (s.state == ST_VALID) {
        if (s.check != chk(&s)) return false;
        if (s.app_end <= BL_APP_START || s.app_end > BL_FLASH_END) return false;
    } else if (s.state != ST_ERASED) {
        return false;
    }
    return app_vectors_sane();
}

void app_state_invalidate(void)
{
    uint32_t zero = 0;
    (void)bl_flash_program(BL_STATE_ADDR, &zero, 4);   /* 1->0 без стирания */
}

bool app_state_commit(uint32_t app_end, uint32_t crc)
{
    state_t s = { ST_VALID, app_end, crc, 0 };
    s.check = chk(&s);
    if (!bl_flash_erase_page(BL_STATE_ADDR)) return false;
    if (!bl_flash_program(BL_STATE_ADDR + 4, &s.app_end, 12)) return false;
    return bl_flash_program(BL_STATE_ADDR, &s.state, 4);
}
