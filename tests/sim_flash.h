#ifndef SIM_FLASH_H
#define SIM_FLASH_H
#include <stdint.h>
#include "bl_config.h"
extern uint8_t  sim_flash[BL_FLASH_SIZE];
extern unsigned sim_erase_count[BL_FLASH_SIZE / BL_PAGE_SIZE];
extern unsigned sim_program_calls;
extern int      sim_fail_erase_page;
extern int      sim_fail_program_after;
void sim_reset(void);
int  sim_bootloader_intact(void);
#endif
