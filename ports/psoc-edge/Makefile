# Get active board from mtb-lib previous runs
# The board is set only after make mtb_init 
# has been run. 
ifeq ($(BOARD),)
    ifeq ($(ACTIVE_BOARD),)
		ifneq ($(MAKECMDGOALS), submodules)
        	$(error ModusToolbox not initialized. Run "make mtb_init BOARD=<target-board>" to configure the environment. )
		endif
    else
        BOARD = $(ACTIVE_BOARD)
    endif
endif

BOARD_DIR  = boards/$(BOARD)

$(info PSOC Edge board        : $(BOARD))

ifeq ($(wildcard $(BOARD_DIR)/.),)
  $(error Invalid BOARD specified)
endif

# Files that are generated and needed before the QSTR build.
QSTR_GENERATED_HEADERS = build/pins_qstr.h
# qstr definitions (must come before including py.mk)
QSTR_DEFS = qstrdefsport.h $(QSTR_GENERATED_HEADERS)
QSTR_GLOBAL_DEPENDENCIES += $(BOARD_DIR)/mpconfigboard.h $(QSTR_GENERATED_HEADERS)

MICROPY_FROZEN_MANIFEST      ?= $(BOARD_DIR)/manifest.py
FROZEN_MANIFEST              ?= $(MICROPY_FROZEN_MANIFEST)


ifneq ($(FROZEN_MANIFEST),)
  CFLAGS += -DMICROPY_QSTR_EXTRA_POOL=mp_qstr_frozen_const_pool
  CFLAGS += -DMICROPY_MODULE_FROZEN_MPY=1
  CFLAGS += -DMICROPY_MODULE_FROZEN_STR=1
endif

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


LDFLAGS += -Wl,--cref -Wl,--gc-sections
LDFLAGS += -Wl,-Map,$(BUILD)/firmware.map  -Wl,--start-group -Wl,--end-group -Wl,--print-memory-usage

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

$(info Compiling in $(CONFIG) mode !)

#ToDo: Post adding af functionality, refactor to minimize dependent variables in py script if possible
GEN_PINS_SRC := $(BUILD)/pins_$(BOARD).c
HEADER_BUILD := $(BUILD)/genhdr
GEN_PINS_HDR := $(BUILD)/genhdr/pins.h
GEN_PINS_QSTR := $(BUILD)/pins_qstr.h

GENERATED_PINS = $(GEN_PINS_SRC) $(GEN_PINS_HDR) $(GEN_PINS_QSTR)

$(GENERATED_PINS): 
	@echo "Generating $@"
	$(MKDIR) -p $(BUILD)/genhdr
# 	$(PYTHON) boards/make-pins.py --gen-pin-for $(PIN_PACKAGE_FILE) --hdr $(GEN_PINS_HDR) --qstr $(GEN_PINS_QSTR) > $(GEN_PINS_SRC) 

# Flags for optional C++ source code
CXXFLAGS += $(filter-out -std=c99,$(CFLAGS))
CXXFLAGS += $(CXXFLAGS_MOD)

LDFLAGS += $(LDFLAGS_MOD)

LIBS += 

SHARED_SRC_C += $(addprefix shared/,\
	readline/readline.c \
	\
	runtime/gchelper_native.c \
	runtime/interrupt_char.c \
	runtime/pyexec.c \
	runtime/mpirq.c\
	runtime/stdout_helpers.c \
	runtime/sys_stdio_mphal.c \
	timeutils/timeutils.c \
	)

DRIVERS_SRC_C += 

MOD_SRC_C +=

SRC_C = help.c \
		main.c \
		mphalport.c 

SRC_ASM +=

SRC_QSTR += $(SHARED_SRC_C) $(MOD_SRC_C)

OBJ += $(PY_O) 
OBJ += $(addprefix $(BUILD)/, $(SHARED_SRC_C:.c=.o)) 
OBJ += $(addprefix $(BUILD)/, $(DRIVERS_SRC_C:.c=.o)) 
OBJ += $(addprefix $(BUILD)/, $(MOD_SRC_C:.c=.o)) 
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o)) 
OBJ += $(addprefix $(BUILD)/, $(SRC_ASM:.s=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_CXX:.cpp=.o))

# switch for debug mode, also added to mpconfigport.h
# TODO: keep the more suitable one, delete the other
MP_LOGGER_DEBUG ?= 0

ifeq ($(MP_LOGGER_DEBUG), 1)
   CFLAGS += -DMICROPY_LOGGER_DEBUG=1
endif

$(BUILD)/firmware.elf: $(OBJ) $(MPY_MTB_LIBRARIES) $(LIBS)
	$(info )
	$(info Linking $@     $^  $(LIBS) ...)
	$(Q) $(LD) $(LDFLAGS) -o $@ $^ $(LIBS)
	$(info Linking $@ done.)
	$(Q) $(SIZE) $@ -A
	$(info )


$(BUILD)/firmware.hex: $(BUILD)/firmware.elf
	$(Q) $(OBJCOPY) -O ihex $^ $@

# include adapter makefile
-include $(TOP)/lib/mtb-psoc-edge-libs/mtb-makefile.mk

MPY_CROSS_FLAGS += -march=armv8m

build_ns: mtb_get_build_flags_ns $(GENERATED_PINS) $(BUILD)/firmware.hex

build_s: mtb_build_s

all: build_ns build_s

rebuild: clean mtb_clean all

deploy: mtb_program

port_help:
	@:
	$(info )
	$(info Available commands:)
	$(info ) 
	$(info   make submodules                    Initializes port required submodules)
	$(info   make mtb_init BOARD=<board_name>   Initialized the ModusToolbox libraries for the specified board.)
	$(info	 ..                                 Only needs to be done once before the first build)
	$(info   make BOARD=<board_name>            Build the project for the specified board)
	$(info   make BOARD=<board_name> deploy     Build and deploy the project to the specified board)
	$(info   make help                          Show this help)
	$(info )

help: port_help mtb_help

.PHONY: all deploy help

include $(TOP)/py/mkrules.mk
