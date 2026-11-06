/* Минимальный набор SCSI-команд для Mass Storage (не зависит от USB). */
#ifndef SCSI_H
#define SCSI_H
#include <stdint.h>
#include <stdbool.h>

typedef enum { SCSI_NODATA = 0, SCSI_DATA_IN, SCSI_READ, SCSI_WRITE } scsi_kind_t;

typedef struct {
    scsi_kind_t kind;
    bool        ok;          /* false -> CHECK CONDITION, sense уже выставлен */
    uint32_t    resp_len;    /* для SCSI_DATA_IN: сколько байт в scsi_resp() */
    uint32_t    lba;         /* для READ/WRITE */
    uint32_t    blocks;
} scsi_cmd_t;

void           scsi_init(void);
void           scsi_parse(const uint8_t cdb[16], scsi_cmd_t *out);
const uint8_t *scsi_resp(void);
bool           scsi_read_sector(uint32_t lba, uint8_t *buf);
bool           scsi_write_sector(uint32_t lba, const uint8_t *buf);
void           scsi_set_sense(uint8_t key, uint8_t asc, uint8_t ascq);
#endif
