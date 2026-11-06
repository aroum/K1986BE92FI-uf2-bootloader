/* Виртуальный FAT16-том с INFO_UF2.TXT и CURRENT.UF2 + приём UF2-блоков. */
#ifndef UF2_FS_H
#define UF2_FS_H
#include <stdint.h>
#include <stdbool.h>

#define UF2_SECTOR_SIZE 512u

uint32_t uf2_fs_sector_count(void);
void     uf2_fs_init(void);
void     uf2_fs_read_sector(uint32_t lba, uint8_t *buf);          /* buf: 512 Б */

/* false -> недопустимый UF2-блок / ошибка Flash (SCSI: MEDIUM ERROR).
 * Любые не-UF2 секторы (метаданные FAT от ОС) молча принимаются. */
bool     uf2_fs_write_sector(uint32_t lba, const uint8_t *buf);

bool     uf2_fs_update_done(void);     /* все блоки приняты, CRC проверен, маркер записан */
bool     uf2_fs_update_busy(void);     /* сессия начата, но не завершена */
#endif
