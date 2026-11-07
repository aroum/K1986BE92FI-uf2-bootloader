TARGET ?= blink
BUILD_DIR ?= build

# Toolchain settings
PREFIX ?= arm-none-eabi-
CC = $(PREFIX)gcc
AS = $(PREFIX)gcc -x assembler-with-cpp
CP = $(PREFIX)objcopy
SZ = $(PREFIX)size
PYTHON ?= python3

# Linker script (custom app offset at 0x08004000)
LDSCRIPT = MDR32F9Q2I_app.ld

# Optimization, CPU and Definitions
MCU = -mcpu=cortex-m3 -mthumb -mfloat-abi=soft
OPT ?= -Os -flto
DEFS = -DMCU_milandr

PLATFORM_DIR = ../platform
COMMON_DIR = $(PLATFORM_DIR)/dep/common
DRIVERS_DIR = $(COMMON_DIR)/Drivers

# C sources
C_SOURCES = \
main.c \
$(DRIVERS_DIR)/CMSIS/DeviceSupport/startup/system_K1986VE9xI.c \
$(DRIVERS_DIR)/SPL/src/MDR32FxQI_rst_clk.c \
$(DRIVERS_DIR)/SPL/src/MDR32FxQI_eeprom.c \
$(DRIVERS_DIR)/SPL/src/MDR32FxQI_bkp.c \
$(DRIVERS_DIR)/SPL/src/MDR32FxQI_port.c

# Assembly sources
ASM_SOURCES_s = \
$(COMMON_DIR)/Startup/startup_gcc_MDR32F9Q2I.s

ASM_SOURCES_S = \
$(DRIVERS_DIR)/SPL/src/MDR32FxQI_asm_GCC.S

# Include paths
C_INCLUDES = \
-I. \
-I../src \
-I$(PLATFORM_DIR) \
-I$(PLATFORM_DIR)/sdk \
-I$(COMMON_DIR)/inc \
-I$(DRIVERS_DIR)/CMSIS/CoreSupport \
-I$(DRIVERS_DIR)/CMSIS/DeviceSupport/inc \
-I$(DRIVERS_DIR)/CMSIS/DeviceSupport/startup \
-I$(DRIVERS_DIR)/SPL \
-I$(DRIVERS_DIR)/SPL/inc \
-I$(DRIVERS_DIR)/SPL/inc/IRQ_Handler_Template

# Compiler and Assembler flags
CFLAGS = $(MCU) $(DEFS) $(C_INCLUDES) $(OPT) -Wall -Wextra \
         -ffunction-sections -fdata-sections -fno-common -fmessage-length=0 \
         -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-deprecated \
         -MMD -MP

ASFLAGS = $(MCU) $(DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections -MMD -MP

# Linker flags
LDFLAGS = $(MCU) -flto -mthumb-interwork --specs=nano.specs -T$(LDSCRIPT) \
          -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections \
          -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments

# Cross-platform helpers for directory creation and removal
ifneq ($(findstring sh,$(SHELL)),)
  MKDIR = mkdir -p $(1)
  RMDIR = rm -rf $(1)
else
  FIX_PATH = $(subst /,\,$(1))
  MKDIR = if not exist "$(call FIX_PATH,$(1))" mkdir "$(call FIX_PATH,$(1))"
  RMDIR = if exist "$(call FIX_PATH,$(1))" rmdir /s /q "$(call FIX_PATH,$(1))"
endif

# Object files mapping (flat inside $(BUILD_DIR))
OBJECTS = $(addprefix $(BUILD_DIR)/, $(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))

OBJECTS += $(addprefix $(BUILD_DIR)/, $(notdir $(ASM_SOURCES_s:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES_s)))

OBJECTS += $(addprefix $(BUILD_DIR)/, $(notdir $(ASM_SOURCES_S:.S=.o)))
vpath %.S $(sort $(dir $(ASM_SOURCES_S)))

# --- Targets ---
.PHONY: all clean help

all: $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).uf2

$(BUILD_DIR):
	@$(call MKDIR,$(BUILD_DIR))

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@echo Compiling $<
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.s | $(BUILD_DIR)
	@echo Assembling $<
	@$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S | $(BUILD_DIR)
	@echo Assembling $<
	@$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS)
	@echo Linking $@
	@$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	@$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf
	@echo Generating $@
	@$(CP) -O ihex $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf
	@echo Generating $@
	@$(CP) -O binary -S $< $@

$(BUILD_DIR)/%.uf2: $(BUILD_DIR)/%.bin
	@echo Generating UF2 $@
	@$(PYTHON) ../tools/bin2uf2.py $< -o $@

clean:
	@$(call RMDIR,$(BUILD_DIR))
	@echo Cleaned $(BUILD_DIR)

help:
	@echo "Available targets:"
	@echo "  make        - Build blink example (ELF, BIN, HEX, UF2)"
	@echo "  make clean  - Remove build artifacts"

-include $(wildcard $(BUILD_DIR)/*.d)
