#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "sim_flash.h"
#include "bl_config.h"
#include "bl_flash.h"
#include "app_state.h"
#include "uf2.h"
#include "uf2_fs.h"
#include "scsi.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define TEST(name) printf("[test] %s\n", name)

static uint32_t rng_state = 12345;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

/* ---------- генератор образа и UF2 (независим от tools/bin2uf2.py) ---------- */
static uint8_t *make_app(uint32_t len)
{
    uint8_t *a = malloc(len);
    for (uint32_t i = 0; i < len; i++) a[i] = (uint8_t)rnd();
    uint32_t sp = BL_RAM_BASE + BL_RAM_SIZE - 16, pc = BL_APP_START + 0x101;
    memcpy(a, &sp, 4); memcpy(a + 4, &pc, 4);
    return a;
}

static uf2_block_t *make_uf2(const uint8_t *app, uint32_t len, uint32_t base, uint32_t payload, uint32_t *nblocks)
{
    uint32_t n = (len + payload - 1) / payload;
    uf2_block_t *b = calloc(n, sizeof *b);
    for (uint32_t i = 0; i < n; i++) {
        b[i].magicStart0 = UF2_MAGIC_START0; b[i].magicStart1 = UF2_MAGIC_START1;
        b[i].magicEnd = UF2_MAGIC_END; b[i].flags = UF2_FLAG_FAMILYID;
        b[i].targetAddr = base + i * payload; b[i].payloadSize = payload;
        b[i].blockNo = i; b[i].numBlocks = n; b[i].familyID = UF2_FAMILY_ID;
        memset(b[i].data, 0xFF, 476);
        uint32_t c = len - i * payload; if (c > payload) c = payload;
        memcpy(b[i].data, app + i * payload, c);
    }
    *nblocks = n;
    return b;
}

static bool wr(const void *blk) { return scsi_write_sector(1000, blk); }
static void fresh(void) { sim_reset(); scsi_init(); }

static void write_all(uf2_block_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) CHECK(wr(&b[i]));
}

static int flash_equals(const uint8_t *app, uint32_t len)
{
    return memcmp(bl_flash_ptr(BL_APP_START), app, len) == 0;
}

static void dump_image(const char *path)
{
    FILE *f = fopen(path, "wb");
    uint8_t s[512];
    for (uint32_t i = 0; i < uf2_fs_sector_count(); i++) { uf2_fs_read_sector(i, s); fwrite(s, 1, 512, f); }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *tool_uf2 = argc > 1 ? argv[1] : NULL;     /* UF2 от tools/bin2uf2.py */
    const char *tool_bin = argc > 2 ? argv[2] : NULL;
    const char *img_path = argc > 3 ? argv[3] : "/tmp/uf2.img";
    uint32_t n;

    /* ------------------------------------------------------------------ */
    TEST("1. полный образ, по порядку");
    fresh();
    CHECK(!app_state_is_bootable());                    /* пустая Flash */
    uint32_t len = BL_APP_SIZE - 777;
    uint8_t *app = make_app(len);
    uf2_block_t *u = make_uf2(app, len, BL_APP_START, 256, &n);
    write_all(u, n);
    CHECK(uf2_fs_update_done());
    CHECK(flash_equals(app, len));
    CHECK(app_state_is_bootable());
    CHECK(sim_bootloader_intact());
    for (uint32_t p = 0; p < BL_FLASH_SIZE / BL_PAGE_SIZE; p++)   /* каждая страница стёрта максимум раз */
        CHECK(sim_erase_count[p] <= 1);
    dump_image(img_path);

    /* ------------------------------------------------------------------ */
    TEST("2. случайный порядок + дубликаты + мусор + чужое семейство");
    fresh();
    uint32_t *order = malloc(n * sizeof *order);
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    for (uint32_t i = n - 1; i > 0; i--) { uint32_t j = rnd() % (i + 1); uint32_t t = order[i]; order[i] = order[j]; order[j] = t; }
    uint8_t junk[512];
    for (uint32_t k = 0; k < n; k++) {
        CHECK(wr(&u[order[k]]));
        if (rnd() % 3 == 0) CHECK(wr(&u[order[k]]));              /* дубликат */
        if (rnd() % 4 == 0) { for (int i = 0; i < 512; i++) junk[i] = (uint8_t)rnd(); CHECK(wr(junk)); }
        if (rnd() % 5 == 0) { uf2_block_t o = u[order[k]]; o.familyID ^= 1; o.targetAddr = BL_APP_START; CHECK(wr(&o)); }
        if (k + 1 < n) CHECK(!uf2_fs_update_done());
    }
    CHECK(uf2_fs_update_done());
    CHECK(flash_equals(app, len));
    CHECK(app_state_is_bootable());
    CHECK(sim_bootloader_intact());
    for (uint32_t p = 0; p < BL_FLASH_SIZE / BL_PAGE_SIZE; p++) CHECK(sim_erase_count[p] <= 1);

    /* ------------------------------------------------------------------ */
    TEST("3. невалидные блоки отвергаются и НЕ ломают рабочее приложение");
    /* состояние после теста 2: приложение валидно */
    uint8_t before[BL_FLASH_SIZE]; memcpy(before, sim_flash, sizeof before);
    struct { const char *name; uint32_t off; uint32_t val; } mut[] = {
        { "addr=bootloader",        offsetof(uf2_block_t, targetAddr), BL_FLASH_BASE },
        { "addr=state page",        offsetof(uf2_block_t, targetAddr), BL_STATE_ADDR },
        { "addr=flash end",         offsetof(uf2_block_t, targetAddr), BL_FLASH_END },
        { "addr=0xFFFFFFFC",        offsetof(uf2_block_t, targetAddr), 0xFFFFFFFCu },
        { "addr=RAM",               offsetof(uf2_block_t, targetAddr), BL_RAM_BASE },
        { "addr straddles end",     offsetof(uf2_block_t, targetAddr), BL_FLASH_END - 128 },
        { "addr unaligned",         offsetof(uf2_block_t, targetAddr), BL_APP_START + 2 },
        { "payload=0",              offsetof(uf2_block_t, payloadSize), 0 },
        { "payload=477",            offsetof(uf2_block_t, payloadSize), 477 },
        { "payload=480 (>476, mult of 4)", offsetof(uf2_block_t, payloadSize), 480 },
        { "payload=4096",           offsetof(uf2_block_t, payloadSize), 4096 },
        { "payload=0xFFFFFFFF",     offsetof(uf2_block_t, payloadSize), 0xFFFFFFFFu },
        { "payload not mult of 4",  offsetof(uf2_block_t, payloadSize), 254 },
        { "numBlocks=0",            offsetof(uf2_block_t, numBlocks), 0 },
        { "numBlocks>max",          offsetof(uf2_block_t, numBlocks), UF2_MAX_BLOCKS + 1 },
        { "blockNo>=numBlocks",     offsetof(uf2_block_t, blockNo), 0xFFFFFFFFu },
    };
    for (unsigned i = 0; i < sizeof mut / sizeof mut[0]; i++) {
        uf2_block_t b = u[5];
        memcpy((uint8_t *)&b + mut[i].off, &mut[i].val, 4);
        if (!(wr(&b) == false)) printf("  not rejected: %s\n", mut[i].name);
        CHECK(wr(&b) == false);
    }
    CHECK(memcmp(before, sim_flash, sizeof before) == 0);   /* Flash не изменилась ни на бит */
    CHECK(app_state_is_bootable());
    /* игнорируемые (не ошибки) */
    { uf2_block_t b = u[5]; b.magicEnd = 0; CHECK(wr(&b)); }
    { uf2_block_t b = u[5]; b.flags |= UF2_FLAG_NOFLASH; b.targetAddr = BL_FLASH_BASE; CHECK(wr(&b)); }
    { uf2_block_t b = u[5]; b.flags &= ~UF2_FLAG_FAMILYID; b.targetAddr = BL_FLASH_BASE; CHECK(wr(&b)); }
    CHECK(memcmp(before, sim_flash, sizeof before) == 0);
    CHECK(sim_bootloader_intact());

    /* ------------------------------------------------------------------ */
    TEST("4. обрыв записи (потеря питания): приложение не загружается");
    fresh();
    for (uint32_t i = 0; i < n / 2; i++) CHECK(wr(&u[i]));   /* вектора уже записаны, остальное нет */
    CHECK(app_vectors_sane());                               /* вектора выглядят правдоподобно... */
    CHECK(!app_state_is_bootable());                         /* ...но загрузка запрещена */
    scsi_init();                                             /* «перезагрузка» загрузчика */
    CHECK(!app_state_is_bootable());
    for (uint32_t i = 0; i < n; i++) CHECK(wr(&u[i]));       /* повторная заливка того же файла */
    CHECK(uf2_fs_update_done() && app_state_is_bootable() && flash_equals(app, len));

    /* ------------------------------------------------------------------ */
    TEST("5. другой файл с тем же numBlocks: новая сессия");
    uint8_t *app2 = make_app(len);
    uf2_block_t *u2 = make_uf2(app2, len, BL_APP_START, 256, &n);
    write_all(u2, n);
    CHECK(uf2_fs_update_done() && app_state_is_bootable() && flash_equals(app2, len));

    /* ------------------------------------------------------------------ */
    TEST("6. round-trip: CURRENT.UF2 -> обратно в устройство");
    uint8_t cur[512 * 512]; uint32_t nb = 0;
    /* найти CURRENT.UF2 через каталог и цепочку FAT (как это делает ОС) */
    {
        uint8_t s[512], root[512], fat[512];
        uf2_fs_read_sector(0, s);
        uint32_t fats = s[16], res = s[14] | (s[15] << 8), fs = s[22] | (s[23] << 8);
        uint32_t rootstart = res + fats * fs;
        uf2_fs_read_sector(rootstart, root);
        uint32_t start = 0, size = 0;
        for (int e = 0; e < 16; e++)
            if (!memcmp(root + e * 32, "CURRENT UF2", 11)) {
                start = root[e * 32 + 26] | (root[e * 32 + 27] << 8);
                memcpy(&size, root + e * 32 + 28, 4);
            }
        CHECK(start >= 2 && size == (BL_APP_SIZE / 256) * 512);
        uint32_t c = start, datastart = rootstart + 4;
        while (c < 0xFFF8) {
            for (int k = 0; k < 4; k++) {
                uint32_t sec = datastart + (c - 2) * 4 + k;
                if (nb < 512) uf2_fs_read_sector(sec, cur + nb * 512);
                nb++;
            }
            uint32_t fsec = res + c / 256;
            uf2_fs_read_sector(fsec, fat);
            c = fat[(c % 256) * 2] | (fat[(c % 256) * 2 + 1] << 8);
        }
        nb = size / 512;
    }
    CHECK(nb == BL_APP_SIZE / 256);
    uint8_t snap[BL_APP_SIZE]; memcpy(snap, bl_flash_ptr(BL_APP_START), BL_APP_SIZE);
    scsi_init();                                             /* новая загрузка загрузчика */
    for (uint32_t i = 0; i < nb; i++) CHECK(wr(cur + i * 512));
    CHECK(uf2_fs_update_done());
    CHECK(memcmp(snap, bl_flash_ptr(BL_APP_START), BL_APP_SIZE) == 0);
    CHECK(app_state_is_bootable());
    CHECK(sim_bootloader_intact());

    /* ------------------------------------------------------------------ */
    TEST("7. малый образ (10 КБ), payload 128 и 476");
    for (int pl = 128; pl <= 476; pl += 348) {
        fresh();
        uint32_t l3 = 10 * 1024;
        uint8_t *a3 = make_app(l3);
        uf2_block_t *u3 = make_uf2(a3, l3, BL_APP_START, (uint32_t)pl, &n);
        write_all(u3, n);
        CHECK(uf2_fs_update_done() && app_state_is_bootable());
        CHECK(memcmp(bl_flash_ptr(BL_APP_START), a3, l3) == 0);
        unsigned erased = 0;
        for (uint32_t p = 0; p < BL_FLASH_SIZE / BL_PAGE_SIZE; p++) erased += sim_erase_count[p] ? 1 : 0;
        CHECK(erased <= 4);                                  /* 3 страницы данных + страница состояния */
        free(a3); free(u3);
    }

    /* ------------------------------------------------------------------ */
    TEST("8. отказ стирания / программирования: нет ложного «успеха»");
    fresh(); sim_fail_erase_page = (int)((BL_APP_START - BL_FLASH_BASE) / BL_PAGE_SIZE + 3);
    int failed_seen = 0;
    {
        uint32_t nn; uf2_block_t *uu = make_uf2(app, len, BL_APP_START, 256, &nn);
        for (uint32_t i = 0; i < nn; i++) if (!wr(&uu[i])) failed_seen = 1;
        CHECK(failed_seen);
        CHECK(!uf2_fs_update_done());
        CHECK(!app_state_is_bootable());
        fresh(); sim_fail_program_after = 20; failed_seen = 0;
        for (uint32_t i = 0; i < nn; i++) if (!wr(&uu[i])) failed_seen = 1;
        CHECK(failed_seen && !uf2_fs_update_done() && !app_state_is_bootable());
        free(uu);
    }

    /* ------------------------------------------------------------------ */
    TEST("9. SCSI");
    fresh();
    scsi_cmd_t c; uint8_t cdb[16];
    memset(cdb, 0, 16); cdb[0] = 0x12; scsi_parse(cdb, &c);
    CHECK(c.ok && c.kind == SCSI_DATA_IN && c.resp_len == 36 && (scsi_resp()[1] & 0x80));
    memset(cdb, 0, 16); cdb[0] = 0x25; scsi_parse(cdb, &c);
    CHECK(c.ok && c.resp_len == 8);
    { const uint8_t *r = scsi_resp(); uint32_t last = (uint32_t)r[0] << 24 | r[1] << 16 | r[2] << 8 | r[3];
      CHECK(last == uf2_fs_sector_count() - 1 && r[6] == 2 && r[7] == 0); }
    memset(cdb, 0, 16); cdb[0] = 0x23; scsi_parse(cdb, &c); CHECK(c.ok && c.resp_len == 12);
    memset(cdb, 0, 16); cdb[0] = 0x28; cdb[5] = 0xFF; cdb[4] = 0xFF; cdb[3] = 0xFF; cdb[2] = 0xFF; cdb[8] = 1; scsi_parse(cdb, &c);
    CHECK(!c.ok);
    memset(cdb, 0, 16); cdb[0] = 0x03; scsi_parse(cdb, &c);
    CHECK(c.ok && c.resp_len == 18 && scsi_resp()[2] == 0x05 && scsi_resp()[12] == 0x21);
    memset(cdb, 0, 16); cdb[0] = 0x03; scsi_parse(cdb, &c);               /* sense очищен */
    CHECK(scsi_resp()[2] == 0);
    memset(cdb, 0, 16); cdb[0] = 0xAA; scsi_parse(cdb, &c); CHECK(!c.ok);
    memset(cdb, 0, 16); cdb[0] = 0x28; cdb[8] = 0;
    cdb[2] = 0; cdb[3] = 0; cdb[4] = 0; cdb[5] = 0; scsi_parse(cdb, &c); CHECK(c.ok && c.blocks == 0);
    {   /* граница: последний сектор читается, следующий — нет */
        uint8_t b[512];
        CHECK(scsi_read_sector(uf2_fs_sector_count() - 1, b));
        CHECK(!scsi_read_sector(uf2_fs_sector_count(), b));
        CHECK(!scsi_write_sector(uf2_fs_sector_count(), b));
    }

    /* ------------------------------------------------------------------ */
    if (tool_uf2 && tool_bin) {
        TEST("10. UF2 от tools/bin2uf2.py (независимая реализация формата)");
        fresh();
        FILE *f = fopen(tool_uf2, "rb"); FILE *g = fopen(tool_bin, "rb");
        CHECK(f && g);
        if (f && g) {
            fseek(g, 0, SEEK_END); long bl = ftell(g); fseek(g, 0, SEEK_SET);
            uint8_t *bin = malloc((size_t)bl); CHECK(fread(bin, 1, (size_t)bl, g) == (size_t)bl);
            uint8_t blk[512]; unsigned cnt = 0;
            while (fread(blk, 1, 512, f) == 512) { CHECK(wr(blk)); cnt++; }
            CHECK(cnt > 0 && uf2_fs_update_done() && app_state_is_bootable());
            CHECK(memcmp(bl_flash_ptr(BL_APP_START), bin, (size_t)bl) == 0);
            free(bin);
        }
        if (f) fclose(f);
        if (g) fclose(g);
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
