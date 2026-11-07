#include <string.h>
#include "scsi.h"
#include "uf2_fs.h"

#define SK_NONE            0x00
#define SK_NOT_READY       0x02
#define SK_MEDIUM_ERROR    0x03
#define SK_ILLEGAL_REQUEST 0x05

static uint8_t sense_key, sense_asc, sense_ascq;
static uint8_t resp[36];

void scsi_set_sense(uint8_t key, uint8_t asc, uint8_t ascq)
{
    sense_key = key; sense_asc = asc; sense_ascq = ascq;
}

void scsi_init(void)
{
    scsi_set_sense(SK_NONE, 0, 0);
    uf2_fs_init();
}

const uint8_t *scsi_resp(void) { return resp; }

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

bool scsi_read_sector(uint32_t lba, uint8_t *buf)
{
    if (lba >= uf2_fs_sector_count()) {
        scsi_set_sense(SK_ILLEGAL_REQUEST, 0x21, 0x00);   /* LBA out of range */
        return false;
    }
    uf2_fs_read_sector(lba, buf);
    return true;
}

bool scsi_write_sector(uint32_t lba, const uint8_t *buf)
{
    if (lba >= uf2_fs_sector_count()) {
        scsi_set_sense(SK_ILLEGAL_REQUEST, 0x21, 0x00);
        return false;
    }
    if (!uf2_fs_write_sector(lba, buf)) {
        scsi_set_sense(SK_MEDIUM_ERROR, 0x0C, 0x00);      /* write error */
        return false;
    }
    return true;
}

void scsi_parse(const uint8_t cdb[16], scsi_cmd_t *o)
{
    memset(o, 0, sizeof *o);
    o->ok = true;

    switch (cdb[0]) {
    case 0x00: /* TEST UNIT READY */
    case 0x1B: /* START STOP UNIT */
    case 0x1E: /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x2F: /* VERIFY(10) */
    case 0x35: /* SYNCHRONIZE CACHE(10) */
        o->kind = SCSI_NODATA;
        break;

    case 0x03: /* REQUEST SENSE */
        memset(resp, 0, 18);
        resp[0] = 0x70; resp[2] = sense_key; resp[7] = 10;
        resp[12] = sense_asc; resp[13] = sense_ascq;
        scsi_set_sense(SK_NONE, 0, 0);
        o->kind = SCSI_DATA_IN; o->resp_len = 18;
        break;

    case 0x12: /* INQUIRY */
        if (cdb[1] & 1) {
            /* EVPD (Vital Product Data) request */
            if (cdb[2] == 0x00) {
                /* Supported VPD Pages (page 0x00) */
                memset(resp, 0, 5);
                resp[0] = 0x00;           /* direct-access */
                resp[1] = 0x00;           /* page code 0x00 */
                resp[3] = 1;              /* page length */
                resp[4] = 0x00;           /* supported page: 0x00 */
                o->kind = SCSI_DATA_IN; o->resp_len = 5;
                break;
            }
            scsi_set_sense(SK_ILLEGAL_REQUEST, 0x24, 0x00); /* invalid field in CDB */
            o->ok = false;
            break;
        }
        memset(resp, 0, 36);
        resp[0] = 0x00;           /* direct-access */
        resp[1] = 0x80;           /* removable */
        resp[2] = 0x02;           /* ANSI SCSI-2 */
        resp[3] = 0x02;           /* response data format */
        resp[4] = 31;
        memcpy(&resp[8],  "Milandr ", 8);
        memcpy(&resp[16], "K1986VE92FI UF2 ", 16);
        memcpy(&resp[32], "1.00", 4);
        o->kind = SCSI_DATA_IN; o->resp_len = 36;
        break;

    case 0x1A: /* MODE SENSE(6) */
        memset(resp, 0, 4);
        resp[0] = 3;
        o->kind = SCSI_DATA_IN; o->resp_len = 4;
        break;

    case 0x5A: /* MODE SENSE(10) */
        memset(resp, 0, 8);
        resp[1] = 6;
        o->kind = SCSI_DATA_IN; o->resp_len = 8;
        break;

    case 0x23: /* READ FORMAT CAPACITIES */
        memset(resp, 0, 12);
        resp[3] = 8;
        put_be32(&resp[4], uf2_fs_sector_count());
        resp[8] = 0x02;           /* formatted media */
        resp[10] = 0x02;          /* 0x000200 = 512 */
        o->kind = SCSI_DATA_IN; o->resp_len = 12;
        break;

    case 0x25: /* READ CAPACITY(10) */
        put_be32(&resp[0], uf2_fs_sector_count() - 1u);
        put_be32(&resp[4], 512u);
        o->kind = SCSI_DATA_IN; o->resp_len = 8;
        break;

    case 0x28: /* READ(10) */
    case 0x2A: /* WRITE(10) */
        o->kind   = (cdb[0] == 0x28) ? SCSI_READ : SCSI_WRITE;
        o->lba    = be32(&cdb[2]);
        o->blocks = ((uint32_t)cdb[7] << 8) | cdb[8];
        if (o->lba >= uf2_fs_sector_count() ||
            o->blocks > uf2_fs_sector_count() - o->lba) {
            scsi_set_sense(SK_ILLEGAL_REQUEST, 0x21, 0x00);
            o->ok = false;
        }
        break;

    default:
        scsi_set_sense(SK_ILLEGAL_REQUEST, 0x20, 0x00);   /* invalid command opcode */
        o->ok = false;
        break;
    }
}
