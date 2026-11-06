#ifndef UF2_H
#define UF2_H
#include <stdint.h>

#define UF2_MAGIC_START0 0x0A324655u
#define UF2_MAGIC_START1 0x9E5D5157u
#define UF2_MAGIC_END    0x0AB16F30u

#define UF2_FLAG_NOFLASH        0x00000001u
#define UF2_FLAG_FILE_CONTAINER 0x00001000u
#define UF2_FLAG_FAMILYID       0x00002000u
#define UF2_FLAG_MD5            0x00004000u
#define UF2_FLAG_EXT_TAGS       0x00008000u

typedef struct {
    uint32_t magicStart0;
    uint32_t magicStart1;
    uint32_t flags;
    uint32_t targetAddr;
    uint32_t payloadSize;
    uint32_t blockNo;
    uint32_t numBlocks;
    uint32_t familyID;
    uint8_t  data[476];
    uint32_t magicEnd;
} uf2_block_t;

_Static_assert(sizeof(uf2_block_t) == 512, "UF2 block must be 512 bytes");
#endif
