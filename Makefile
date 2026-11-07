# UF2-загрузчик K1986BE92FI. Использует включённые в репозиторий SPL/CMSIS.
PLATFORM_DIR ?= platform
TARGET ?= k1986ve92fi_uf2_bootloader
BUILD_DIR ?= build

PREFIX ?= arm-none-eabi-
CC = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
SIZE = $(PREFIX)size

D = $(PLATFORM_DIR)/dep/common
SPL = $(D)/Drivers/SPL

C_SOURCES = \
  src/main.c src/bl_flash.c src/usb_msc.c src/scsi.c src/uf2_fs.c src/app_state.c \
  $(SPL)/src/MDR32FxQI_eeprom.c \
  $(SPL)/src/MDR32FxQI_usb.c \
  $(SPL)/src/USB_Library/MDR32FxQI_usb_device.c \
  $(D)/Drivers/CMSIS/DeviceSupport/startup/system_K1986VE9xI.c

ASM_S   = $(D)/Startup/startup_gcc_MDR32F9Q2I.s
ASM_BIG = $(SPL)/src/MDR32FxQI_asm_GCC.S

# ВАЖНО: -Isrc первым — наш MDR32FxQI_usb_handlers.h должен перекрыть версию из SDK.
INCLUDES = -Isrc -I$(PLATFORM_DIR) -I$(PLATFORM_DIR)/sdk -I$(PLATFORM_DIR)/sdk/USB_Library \
  -I$(D)/inc -I$(D)/Drivers/CMSIS/CoreSupport -I$(D)/Drivers/CMSIS/DeviceSupport/inc \
  -I$(D)/Drivers/CMSIS/DeviceSupport/startup -I$(SPL) -I$(SPL)/inc \
  -I$(SPL)/inc/IRQ_Handler_Template -I$(SPL)/inc/USB_Library

MCU = -mcpu=cortex-m3 -mthumb -mfloat-abi=soft
OPT ?= -Os -flto
CFLAGS = $(MCU) $(INCLUDES) $(OPT) -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -ffunction-sections -fdata-sections -fno-common -fno-strict-aliasing -MMD -MP
ASFLAGS = $(MCU) $(INCLUDES) $(OPT) -x assembler-with-cpp -MMD -MP
LDFLAGS = $(MCU) $(OPT) -nostartfiles --specs=nano.specs -Tbootloader.ld \
  -Wl,-Map=$(BUILD_DIR)/$(TARGET).map -Wl,--gc-sections -Wl,--print-memory-usage \
  -Wl,--no-warn-rwx-segments

# Cross-platform helpers for directory creation and removal
ifneq ($(findstring sh,$(SHELL)),)
  MKDIR = mkdir -p $(1)
  RMDIR = rm -rf $(1)
else
  FIX_PATH = $(subst /,\,$(1))
  MKDIR = if not exist "$(call FIX_PATH,$(1))" mkdir "$(call FIX_PATH,$(1))"
  RMDIR = if exist "$(call FIX_PATH,$(1))" rmdir /s /q "$(call FIX_PATH,$(1))"
endif

OBJS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o))) \
       $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_S:.s=.o))) \
       $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_BIG:.S=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
vpath %.s $(sort $(dir $(ASM_S)))
vpath %.S $(sort $(dir $(ASM_BIG)))

all: bootloader

bootloader: $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex
	@$(SIZE) $(BUILD_DIR)/$(TARGET).elf

blink:
	$(MAKE) -C example_blink

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/%.o: %.s | $(BUILD_DIR)
	$(CC) -c $(ASFLAGS) $< -o $@
$(BUILD_DIR)/%.o: %.S | $(BUILD_DIR)
	$(CC) -c $(ASFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJS) bootloader.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@
$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD_DIR):
	@$(call MKDIR,$(BUILD_DIR))

# Хост-тесты логики FAT/UF2/SCSI (без железа)
test:
	$(MAKE) -C tests run

clean:
	@$(call RMDIR,$(BUILD_DIR))
	$(MAKE) -C tests clean

clean-all: clean
	$(MAKE) -C example_blink clean

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all bootloader blink test clean clean-all
