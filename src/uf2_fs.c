#include <string.h>
#include "uf2_fs.h"
#include "uf2.h"
#include "bl_config.h"
#include "bl_flash.h"
#include "app_state.h"

/* ------------------------------------------------------------------------- */
/*  Геометрия тома (FAT16, 512 Б сектор, 4 сектора на кластер)                */
/* ------------------------------------------------------------------------- */
#define SECTOR            512u
#define SEC_PER_CLUSTER   4u
#define RESERVED_SECTORS  1u
#define NUM_FATS          2u
#define ROOT_ENTRIES      64u
#define TOTAL_SECTORS     0x10109u
#define ROOT_SECTORS      (ROOT_ENTRIES * 32u / SECTOR)
#define CLUSTERS_ROUNDUP  ((TOTAL_SECTORS + SEC_PER_CLUSTER - 1u) / SEC_PER_CLUSTER)
#define FAT_SECTORS       ((CLUSTERS_ROUNDUP + 255u) / 256u)
#define FAT0_START        RESERVED_SECTORS
#define FAT1_START        (FAT0_START + FAT_SECTORS)
#define ROOT_START        (FAT1_START + FAT_SECTORS)
#define DATA_START        (ROOT_START + ROOT_SECTORS)
#define CLUSTER_COUNT     ((TOTAL_SECTORS - DATA_START) / SEC_PER_CLUSTER)
#define CLUSTER_BYTES     (SECTOR * SEC_PER_CLUSTER)

/* Валидный FAT16 с запасом от граничных значений (часть ОС ошибается на ±N). */
_Static_assert(CLUSTER_COUNT >= 4085u + 32u && CLUSTER_COUNT < 65525u - 32u, "not FAT16");

#define UF2_FILE_BLOCKS   (BL_APP_SIZE / UF2_PAYLOAD_SIZE)
_Static_assert(BL_APP_SIZE % UF2_PAYLOAD_SIZE == 0, "app size must be multiple of payload");
_Static_assert((BL_APP_SIZE / BL_PAGE_SIZE) <= 32, "erased-page bitmap is 32 bits");

#pragma pack(push, 1)
typedef struct {
    uint8_t  jump[3];
    uint8_t  oem[8];
    uint16_t sector_size;
    uint8_t  sec_per_cluster;
    uint16_t reserved;
    uint8_t  fats;
    uint16_t root_entries;
    uint16_t total16;
    uint8_t  media;
    uint16_t fat_sectors;
    uint16_t sec_per_track;
    uint16_t heads;
    uint32_t hidden;
    uint32_t total32;
    uint8_t  drive;
    uint8_t  reserved1;
    uint8_t  ext_sig;
    uint32_t serial;
    uint8_t  label[11];
    uint8_t  fstype[8];
} boot_sector_t;

typedef struct {
    char     name[11];
    uint8_t  attr;
    uint8_t  nt;
    uint8_t  ctime_fine;
    uint16_t ctime, cdate, adate, cluster_hi, mtime, mdate, cluster_lo;
    uint32_t size;
} dir_entry_t;
#pragma pack(pop)
_Static_assert(sizeof(dir_entry_t) == 32, "dir entry");

#define VOLUME_LABEL "K1986VE92FI"   /* ровно 11 символов */
#define DOS_DATE     ((2026u - 1980u) << 9 | 1u << 5 | 1u)

static const char info_txt[] =
    "UF2 Bootloader " BL_VERSION "\r\n"
    "Model: K1986VE92FI\r\n"
    "Board-ID: K1986VE92FI-v1\r\n"
    "Family-ID: 0xE47E1D64\r\n"
    "App-Start: 0x08004000\r\n";

typedef struct {
    const char *name;           /* 11 символов, 8.3 без точки */
    const uint8_t *content;     /* NULL -> CURRENT.UF2 (генерируется) */
    uint32_t size;
    uint32_t c_start, c_end;
} file_t;

static file_t files[] = {
    { "INFO_UF2TXT", (const uint8_t *)info_txt, sizeof(info_txt) - 1, 0, 0 },
    { "CURRENT UF2", 0, UF2_FILE_BLOCKS * SECTOR, 0, 0 },
};
#define NUM_FILES (sizeof(files) / sizeof(files[0]))
#define FID_UF2   1u

static uint32_t div_ceil(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }

uint32_t uf2_fs_sector_count(void) { return TOTAL_SECTORS; }

/* ------------------------------------------------------------------------- */
/*  Состояние сессии записи                                                   */
/* ------------------------------------------------------------------------- */
static struct {
    bool     active;
    bool     failed;
    bool     done;
    uint32_t num_blocks;
    uint32_t num_written;
    uint32_t app_end;
    uint32_t erased_pages;                     /* бит i = страница i приложения стёрта */
    uint8_t  written[UF2_MAX_BLOCKS / 8];
} S;

void uf2_fs_init(void)
{
    uint32_t c = 2;
    for (unsigned i = 0; i < NUM_FILES; i++) {
        files[i].c_start = c;
        files[i].c_end   = c + div_ceil(files[i].size, CLUSTER_BYTES) - 1u;
        c = files[i].c_end + 1u;
    }
    memset(&S, 0, sizeof S);
}

bool uf2_fs_update_done(void) { return S.done; }
bool uf2_fs_update_busy(void) { return S.active && !S.done; }

/* ------------------------------------------------------------------------- */
/*  Чтение                                                                    */
/* ------------------------------------------------------------------------- */
static void put16(uint8_t *b, uint32_t idx, uint16_t v)
{
    b[idx * 2] = (uint8_t)v;
    b[idx * 2 + 1] = (uint8_t)(v >> 8);
}

static void make_boot_sector(uint8_t *buf)
{
    boot_sector_t bs;
    memset(&bs, 0, sizeof bs);
    bs.jump[0] = 0xEB; bs.jump[1] = 0x3C; bs.jump[2] = 0x90;
    memcpy(bs.oem, "UF2 UF2 ", 8);
    bs.sector_size     = SECTOR;
    bs.sec_per_cluster = SEC_PER_CLUSTER;
    bs.reserved        = RESERVED_SECTORS;
    bs.fats            = NUM_FATS;
    bs.root_entries    = ROOT_ENTRIES;
    bs.total16         = (TOTAL_SECTORS > 0xFFFFu) ? 0 : (uint16_t)TOTAL_SECTORS;
    bs.media           = 0xF8;
    bs.fat_sectors     = FAT_SECTORS;
    bs.sec_per_track   = 1;
    bs.heads           = 1;
    bs.total32         = (TOTAL_SECTORS > 0xFFFFu) ? TOTAL_SECTORS : 0;
    bs.drive           = 0x80;
    bs.ext_sig         = 0x29;
    bs.serial          = 0x00420042u;
    memcpy(bs.label, VOLUME_LABEL, 11);
    memcpy(bs.fstype, "FAT16   ", 8);
    memcpy(buf, &bs, sizeof bs);
    buf[510] = 0x55;
    buf[511] = 0xAA;
}

static void make_fat_sector(uint32_t rel, uint8_t *buf)
{
    uint32_t first = rel * 256u;
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = first + i;
        uint16_t v = 0;
        if (c == 0)      v = 0xFFF8;
        else if (c == 1) v = 0xFFFF;
        else {
            for (unsigned f = 0; f < NUM_FILES; f++) {
                if (c >= files[f].c_start && c <= files[f].c_end) {
                    v = (c == files[f].c_end) ? 0xFFFFu : (uint16_t)(c + 1u);
                    break;
                }
            }
        }
        put16(buf, i, v);
    }
}

static void make_root_sector(uint32_t rel, uint8_t *buf)
{
    if (rel != 0) return;                       /* все записи помещаются в первый сектор */
    dir_entry_t *d = (dir_entry_t *)(void *)buf;
    memcpy(d->name, VOLUME_LABEL, 11);
    d->attr = 0x28;                             /* volume label | archive */
    d++;
    for (unsigned f = 0; f < NUM_FILES; f++, d++) {
        memcpy(d->name, files[f].name, 11);
        d->cdate = d->adate = d->mdate = DOS_DATE;
        d->cluster_hi = 0;
        d->cluster_lo = (uint16_t)files[f].c_start;
        d->size = files[f].size;
    }
}

static void make_uf2_block(uint32_t index, uint8_t *buf)
{
    if (index >= UF2_FILE_BLOCKS) return;
    uf2_block_t *b = (uf2_block_t *)(void *)buf;
    uint32_t addr = BL_APP_START + index * UF2_PAYLOAD_SIZE;
    b->magicStart0 = UF2_MAGIC_START0;
    b->magicStart1 = UF2_MAGIC_START1;
    b->magicEnd    = UF2_MAGIC_END;
    b->flags       = UF2_FLAG_FAMILYID;
    b->targetAddr  = addr;
    b->payloadSize = UF2_PAYLOAD_SIZE;
    b->blockNo     = index;
    b->numBlocks   = UF2_FILE_BLOCKS;
    b->familyID    = UF2_FAMILY_ID;
    memcpy(b->data, bl_flash_ptr(addr), UF2_PAYLOAD_SIZE);
}

void uf2_fs_read_sector(uint32_t lba, uint8_t *buf)
{
    memset(buf, 0, SECTOR);
    if (lba == 0) {
        make_boot_sector(buf);
    } else if (lba < ROOT_START) {
        uint32_t rel = lba - FAT0_START;
        if (rel >= FAT_SECTORS) rel -= FAT_SECTORS;      /* вторая копия FAT идентична */
        make_fat_sector(rel, buf);
    } else if (lba < DATA_START) {
        make_root_sector(lba - ROOT_START, buf);
    } else if (lba < TOTAL_SECTORS) {
        uint32_t rel = lba - DATA_START;
        uint32_t cluster = 2u + rel / SEC_PER_CLUSTER;
        for (unsigned f = 0; f < NUM_FILES; f++) {
            if (cluster < files[f].c_start || cluster > files[f].c_end) continue;
            uint32_t fsec = rel - (files[f].c_start - 2u) * SEC_PER_CLUSTER;
            if (files[f].content) {
                uint32_t off = fsec * SECTOR;
                if (off < files[f].size) {
                    uint32_t n = files[f].size - off;
                    memcpy(buf, files[f].content + off, n > SECTOR ? SECTOR : n);
                }
            } else {
                make_uf2_block(fsec, buf);
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Запись UF2                                                                */
/* ------------------------------------------------------------------------- */
static void new_session(uint32_t num_blocks)
{
    memset(&S, 0, sizeof S);
    S.active = true;
    S.num_blocks = num_blocks;
    app_state_invalidate();           /* с этого момента приложение не загружается */
}

static bool program_block(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t first = (addr - BL_APP_START) / BL_PAGE_SIZE;
    uint32_t last  = (addr + len - 1u - BL_APP_START) / BL_PAGE_SIZE;
    for (uint32_t p = first; p <= last; p++) {
        if (S.erased_pages & (1u << p)) continue;
        if (!bl_flash_erase_page(BL_APP_START + p * BL_PAGE_SIZE)) return false;
        S.erased_pages |= 1u << p;
    }
    return bl_flash_program(addr, data, len);
}

static bool finish_session(void)
{
    uint32_t end = S.app_end;
    uint32_t crc = app_crc32(BL_APP_START, end - BL_APP_START);
    if (!app_state_commit(end, crc)) return false;
    return app_state_is_bootable();   /* векторы приложения правдоподобны */
}

bool uf2_fs_write_sector(uint32_t lba, const uint8_t *buf)
{
    (void)lba;                                   /* UF2-блоки распознаются по сигнатуре, а не по LBA */
    uf2_block_t b;
    memcpy(&b, buf, sizeof b);

    if (b.magicStart0 != UF2_MAGIC_START0 || b.magicStart1 != UF2_MAGIC_START1 ||
        b.magicEnd != UF2_MAGIC_END)
        return true;                             /* метаданные ФС — игнорируем */
    if (!(b.flags & UF2_FLAG_FAMILYID) || (b.flags & (UF2_FLAG_NOFLASH | UF2_FLAG_FILE_CONTAINER)))
        return true;                             /* не для флеш-записи */
    if (b.familyID != UF2_FAMILY_ID)
        return true;                             /* блок для другого семейства (спецификация UF2) */

    /* ---- строгая проверка до любых побочных эффектов ---- */
    if (b.payloadSize == 0 || b.payloadSize > UF2_MAX_PAYLOAD || (b.payloadSize & 3u)) return false;
    if (b.targetAddr & 3u) return false;
    if (b.targetAddr < BL_APP_START || b.targetAddr > BL_FLASH_END - b.payloadSize) return false;
    if (b.numBlocks == 0 || b.numBlocks > UF2_MAX_BLOCKS || b.blockNo >= b.numBlocks) return false;

    if (!S.active || S.num_blocks != b.numBlocks) new_session(b.numBlocks);

    uint32_t idx = b.blockNo;
    if (S.written[idx / 8] & (1u << (idx % 8))) {
        if (memcmp(bl_flash_ptr(b.targetAddr), b.data, b.payloadSize) == 0)
            return !S.failed;                    /* повтор того же блока (ОС любит перезаписывать) */
        new_session(b.numBlocks);                /* тот же номер, другое содержимое -> новый файл */
    }

    if (!program_block(b.targetAddr, b.data, b.payloadSize)) {
        S.failed = true;
        return false;
    }
    S.written[idx / 8] |= (uint8_t)(1u << (idx % 8));
    S.num_written++;
    if (b.targetAddr + b.payloadSize > S.app_end) S.app_end = b.targetAddr + b.payloadSize;

    if (S.num_written == S.num_blocks && !S.failed) {
        if (!finish_session()) { S.failed = true; return false; }
        S.done = true;
    }
    return true;
}
