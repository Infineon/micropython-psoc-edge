MTB_LIBS_DIR = ../../lib/mtb-psoc-edge-libs
-include $(MTB_LIBS_DIR)/mtb-bsp-setup.mk

# The only target allowing to run without BOARD set is 'submodules' or 'help'
ifeq ($(BOARD),)
	ifneq ($(filter $(MAKECMDGOALS),submodules help),$(MAKECMDGOALS))
    	$(error No active board is set. Run "make BOARD=<target-board>" to initialize the ModusToolbox libraries and set the active board. )
	endif
endif

# Files that are generated and needed before the QSTR build.
QSTR_GENERATED_HEADERS = build/pins_qstr.h
# # qstr definitions (must come before including py.mk)
# QSTR_DEFS = qstrdefsport.h $(QSTR_GENERATED_HEADERS)
# QSTR_GLOBAL_DEPENDENCIES += $(BOARD_DIR)/mpconfigboard.h $(QSTR_GENERATED_HEADERS)

# MICROPY_FROZEN_MANIFEST      ?= $(BOARD_DIR)/manifest.py
# FROZEN_MANIFEST              ?= $(MICROPY_FROZEN_MANIFEST)


# ifneq ($(FROZEN_MANIFEST),)
#   CFLAGS += -DMICROPY_QSTR_EXTRA_POOL=mp_qstr_frozen_const_pool
#   CFLAGS += -DMICROPY_MODULE_FROZEN_MPY=1
#   CFLAGS += -DMICROPY_MODULE_FROZEN_STR=1
# endif

CROSS_COMPILE ?= arm-none-eabi-
CONFIG        ?= Debug

include ../../py/mkenv.mk
-include $(BOARD_DIR)/mpconfigboard.mk
include $(TOP)/py/py.mk
include $(TOP)/extmod/extmod.mk

GIT_SUBMODULES += lib/mtb-psoc-edge-libs

INC += -I.
INC += -I$(TOP)
INC += -I$(BUILD)
INC += -I$(BUILD)

LD               = arm-none-eabi-gcc
CFLAGS_CORTEX_M33 = -mthumb -mcpu=cortex-m33 -msoft-float -fsingle-precision-constant -Wdouble-promotion -Wfloat-conversion -UMICROPY_USE_INTERNAL_PRINTF -Wno-error=float-conversion

# std=c11 instead of std=c99 : provides "static_assert" (not available in c99) 
# -D_XOPEN_SOURCE=700 : makes sure the setenv/unsetenv headers are included
CFLAGS  += $(INC) -Wall -Werror -std=c11 $(CFLAGS_CORTEX_M33) $(COPT) -D_XOPEN_SOURCE=700
CFLAGS  += -Wno-error=double-promotion -Wno-error=overflow -Wno-error=analyzer-null-dereference -Wno-error=unused-local-typedefs -Wno-error=unused-function -Wno-error=maybe-uninitialized


# LDFLAGS += -Wl,--cref -Wl,--gc-sections
# LDFLAGS += -Wl,-Map,$(BUILD)/firmware.map  -Wl,--start-group -Wl,--end-group -Wl,--print-memory-usage

# Tune for Debugging or Optimization
ifeq ($(CONFIG), Debug)
  CFLAGS         += -O0 -ggdb
  MPY_MTB_CONFIG  = Debug
  MICROPY_ROM_TEXT_COMPRESSION ?= 0
else
  CFLAGS         += -O3 -Os -DNDEBUG
  CFLAGS         += -fdata-sections -ffunction-sections
  MPY_MTB_CONFIG  = Release
  MICROPY_ROM_TEXT_COMPRESSION ?= 1
endif

# $(info Compiling in $(CONFIG) mode )

#ToDo: Post adding af functionality, refactor to minimize dependent variables in py script if possible
# GEN_PINS_SRC := $(BUILD)/pins_$(BOARD).c
# HEADER_BUILD := $(BUILD)/genhdr
# GEN_PINS_HDR := $(BUILD)/genhdr/pins.h
# GEN_PINS_QSTR := $(BUILD)/pins_qstr.h

# GENERATED_PINS = $(GEN_PINS_SRC) $(GEN_PINS_HDR) $(GEN_PINS_QSTR)

# $(GENERATED_PINS): 
# 	@echo "Generating $@"
# 	$(MKDIR) -p $(BUILD)/genhdr
# 	$(PYTHON) boards/make-pins.py --gen-pin-for $(PIN_PACKAGE_FILE) --hdr $(GEN_PINS_HDR) --qstr $(GEN_PINS_QSTR) > $(GEN_PINS_SRC) 

# Flags for optional C++ source code
# CXXFLAGS += $(filter-out -std=c99,$(CFLAGS))
# CXXFLAGS += $(CXXFLAGS_MOD)

# LDFLAGS += $(LDFLAGS_MOD)

LIBS += 

SHARED_SRC_C += $(addprefix shared/,\
	readline/readline.c \
	\
	runtime/gchelper_native.c \
	runtime/pyexec.c \
	runtime/stdout_helpers.c \
	runtime/sys_stdio_mphal.c \
	timeutils/timeutils.c \
	)

# 	libc/printf.c \
# 	runtime/interrupt_char.c \
# 	runtime/mpirq.c\

DRIVERS_SRC_C += 

MOD_SRC_C +=

# SRC_C = help.c \
# 		main.c \
# 		mphalport.c 

MTB_C = main.c

SRC_ASM +=

SRC_QSTR += $(SHARED_SRC_C) $(MOD_SRC_C)

# OBJ += $(PY_O) 
# OBJ += $(addprefix $(BUILD)/, $(SHARED_SRC_C:.c=.o)) 
# OBJ += $(addprefix $(BUILD)/, $(DRIVERS_SRC_C:.c=.o)) 
# OBJ += $(addprefix $(BUILD)/, $(MOD_SRC_C:.c=.o)) 
# OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o)) 
# OBJ += $(addprefix $(BUILD)/, $(SRC_ASM:.s=.o))
# OBJ += $(addprefix $(BUILD)/, $(SRC_CXX:.cpp=.o))

MTB_OBJ = $(addprefix $(BUILD)/, $(MTB_C:.c=.o))

# # switch for debug mode, also added to mpconfigport.h
# # TODO: keep the more suitable one, delete the other
# MP_LOGGER_DEBUG ?= 0

# ifeq ($(MP_LOGGER_DEBUG), 1)
#    CFLAGS += -DMICROPY_LOGGER_DEBUG=1
# endif

-include $(TOP)/lib/mtb-psoc-edge-libs/mtb-makefile.mk

MPY_CROSS_FLAGS += -march=armv8-m.main

# The creation of the build directory is only temporary. Removed once created by mpy makefile system.
create_build_dir:
	$(Q) mkdir -p $(BUILD)

$(BUILD)/mpy_edge_app_ns.elf: create_build_dir mtb_build_ns mtb_get_build_flags_ns $(MTB_OBJ) 
	$(info )
	$(info Linking $@ ...)
	arm-none-eabi-g++ -mcpu=cortex-m33 --specs=nano.specs -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -mthumb -ffunction-sections -fdata-sections -ffat-lto-objects -g -Wall -pipe -Wl,--gc-sections -Wl,--print-memory-usage -Xlinker -L ../../lib/mtb-psoc-edge-libs/bsps/TARGET_APP_KIT_PSE84_AI/COMPONENT_CM33/TOOLCHAIN_GCC_ARM -Xlinker -L ../../lib/mtb-psoc-edge-libs/bsps/TARGET_APP_KIT_PSE84_AI/config/GeneratedSource -T../../lib/mtb-psoc-edge-libs/bsps/TARGET_APP_KIT_PSE84_AI/COMPONENT_CM33/TOOLCHAIN_GCC_ARM/pse84_ns_cm33.ld -Wl,-Map,/home/enriquezgarc/micropython-psoc-edge/lib/mtb-psoc-edge-libs/proj_cm33_ns/build/APP_KIT_PSE84_AI/Debug/proj_cm33_ns.map -Wl,--start-group ../../lib/mtb-psoc-edge-libs/proj_cm33_s/nsc_veneer.o -Wl,--end-group -o $@ build/main.o ../../lib/mtb-psoc-edge-libs/proj_cm33_ns/build/APP_KIT_PSE84_AI/Debug/proj_cm33_ns.a
	$(info Linking $@ done.)
	$(Q) $(SIZE) $@ -A
	$(info )

build_mpy_app_ns: $(BUILD)/mpy_edge_app_ns.hex
$(BUILD)/mpy_edge_app_ns.hex: $(BUILD)/mpy_edge_app_ns.elf
	$(Q) $(OBJCOPY) -O ihex $^ $@

   																		 -mcpu=cortex-m33 --specs=nano.specs -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -mthumb -ffunction-sections -fdata-sections -ffat-lto-objects -g -Wall -pipe -Wl,--gc-sections -Wl,--print-memory-usage -Xlinker -L ../../lib/mtb-psoc-edge-libs/bsps/TARGET_APP_KIT_PSE84_AI/COMPONENT_CM33/TOOLCHAIN_GCC_ARM -Xlinker -L ../../lib/mtb-psoc-edge-libs/bsps/TARGET_APP_KIT_PSE84_AI/config/GeneratedSource -T"../../lib/mtb-psoc-edge-libs/bsps/TARGET_APP_KIT_PSE84_AI/COMPONENT_CM33/TOOLCHAIN_GCC_ARM/pse84_ns_cm33.ld" -Wl,-Map,@mapfile                                                                                        -Wl,--start-group                                                 -Wl,--end-group -o @elffile -o build/mpy_ns.elf  ../../lib/mtb-psoc-edge-libs/proj_cm33_ns/build/APP_KIT_PSE84_AI/Debug/proj_cm33_ns.a
all: mtb_init build_mpy_app_ns mtb_build_s

rebuild: clean all

clean: mtb_clean

qdeploy: mtb_program
deploy: all mtb_program

port_help:
	@:
	$(info )
	$(info Available commands:)
	$(info ) 
	$(info   make submodules                    Initialize port required submodules.)
	$(info   make BOARD=<board_name>            Build the project for the specified board.)
	$(info   ..                                 The board name needs to be specified only the first time.)
	$(info   ..                                 Then simply run "make" to build for the same board.)
	$(info   make rebuild                       Build the project after cleaning previous build.)
	$(info   make deploy                        Build and deploy the project to the specified board.)
	$(info   make qdeploy                       Deploy the project to the specified board without rebuilding.)
	$(info   make clean                         Clean the build files.)
	$(info   make help                          Show this help.)
	$(info )

help: port_help mtb_bsp_help
.DEFAULT_GOAL := all

.PHONY: all clean rebuild deploy help

include $(TOP)/py/mkrules.mk
