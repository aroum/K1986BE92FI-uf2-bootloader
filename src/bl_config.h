/* Конфигурация UF2-загрузчика K1986BE92FI (MDR32F9Q2I-совместимое ядро).
 * Файл не зависит от SPL — им пользуются и прошивка, и хост-тесты. */
#ifndef BL_CONFIG_H
#define BL_CONFIG_H

#define BL_VERSION          "1.0.0"

/* ---- Разметка Flash (128 КБ, страница стирания 4 КБ) -------------------- */
#define BL_FLASH_BASE       0x08000000u
#define BL_FLASH_SIZE       0x00020000u
#define BL_PAGE_SIZE        0x00001000u
#define BL_CODE_SIZE        0x00003000u                    /* код загрузчика: 12 КБ (3 страницы) */
#define BL_STATE_ADDR       (BL_FLASH_BASE + BL_CODE_SIZE) /* 1 страница: маркер валидности приложения */
#define BL_APP_START        (BL_STATE_ADDR + BL_PAGE_SIZE) /* 0x08004000 */
#define BL_FLASH_END        (BL_FLASH_BASE + BL_FLASH_SIZE)
#define BL_APP_SIZE         (BL_FLASH_END - BL_APP_START)  /* 112 КБ */

#define BL_RAM_BASE         0x20000000u
#define BL_RAM_SIZE         0x00008000u

/* ---- UF2 ---------------------------------------------------------------- */
/* Family ID НЕ зарегистрирован в microsoft/uf2. Значение = первые 4 байта
 * sha256("K1986VE92FI") (little-endian). Для публичного продукта зарегистрируйте. */
#define UF2_FAMILY_ID       0xE47E1D64u
#define UF2_PAYLOAD_SIZE    256u         /* размер payload в CURRENT.UF2 */
#define UF2_MAX_BLOCKS      2048u        /* максимум блоков в одном .uf2 (битовая карта 256 Б) */
#define UF2_MAX_PAYLOAD     476u

#define BL_RESET_DELAY_MS   500u         /* пауза после завершения записи до сброса */

/* ---- USB ---------------------------------------------------------------- */
#define BL_USB_VID          0x1209u      /* та же отладочная пара, что и в DFU-загрузчике */
#define BL_USB_PID          0xBE92u

#endif
