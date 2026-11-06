/* Состояние приложения: маркер валидности в отдельной странице + CRC32. */
#ifndef APP_STATE_H
#define APP_STATE_H
#include <stdint.h>
#include <stdbool.h>

uint32_t app_crc32(uint32_t start, uint32_t len);
bool     app_vectors_sane(void);         /* SP/PC в допустимых диапазонах */
bool     app_state_is_bootable(void);    /* можно ли передавать управление приложению */
void     app_state_invalidate(void);     /* начало обновления: приложение недействительно */
bool     app_state_commit(uint32_t app_end, uint32_t crc); /* конец обновления */
#endif
