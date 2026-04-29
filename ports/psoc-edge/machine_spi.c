/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022-2026 Infineon Technologies AG
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/runtime.h"
#include "extmod/modmachine.h"
#include <stdio.h>
#include <string.h>
#include "py/mphal.h"
#include "py/mperrno.h"
#include "cybsp.h"
#include "cy_scb_spi.h"
#include "cy_sysclk.h"
#include "genhdr/pins_af.h"
#include "mplogger.h"
#include "machine_scb.h"

// Port-specific include
#include "modmachine.h"

#define DEFAULT_SPI_BAUDRATE    (115200)
#define DEFAULT_SPI_POLARITY    (0)
#define DEFAULT_SPI_PHASE       (0)
#define DEFAULT_SPI_BITS        (8)
#define DEFAULT_SPI_TIMEOUT     (50000)

#define SPI_OVERSAMPLE          (4U)
#define SPI_CLK_DIV_TYPE        CY_SYSCLK_DIV_8_BIT
#define SPI_CLK_DIV_BASE        (3U)

typedef struct {
    uint32_t peri_nr;
    uint32_t group_nr;
    uint32_t slave_nr;
    uint32_t clk_hf_nr;
} machine_scb_group_info_t;

// The entry is indexed by SCB unit number and later used to enable
// the correct peripheral group/clock path for that SCB.
#define SCB_GROUP_ENTRY(n) \
    [n] = { \
        .peri_nr = CY_MMIO_SCB##n##_PERI_NR, \
        .group_nr = CY_MMIO_SCB##n##_GROUP_NR, \
        .slave_nr = CY_MMIO_SCB##n##_SLAVE_NR, \
        .clk_hf_nr = CY_MMIO_SCB##n##_CLK_HF_NR, \
    },

static const machine_scb_group_info_t scb_group_table[] = {
    MICROPY_PY_MACHINE_FOR_ALL_SCB(SCB_GROUP_ENTRY)
};

typedef struct _machine_spi_obj_t {
    mp_obj_base_t base;
    mp_hal_pin_obj_t sck;
    mp_hal_pin_obj_t mosi;
    mp_hal_pin_obj_t miso;
    mp_hal_pin_obj_t ssel;
    bool has_ssel;
    bool is_slave;
    uint32_t baudrate;
    uint8_t polarity;
    uint8_t phase;
    uint8_t bits;
    uint8_t firstbit;
    uint32_t timeout;
    uint8_t div_num;
    machine_scb_obj_t *scb_obj;
    cy_stc_scb_spi_config_t cfg;
    cy_stc_scb_spi_context_t ctx;
} machine_spi_obj_t;

machine_spi_obj_t *machine_hw_spi_obj[MICROPY_PY_MACHINE_SPI_NUM_ENTRIES] = { NULL };

static inline machine_spi_obj_t *machine_hw_spi_obj_alloc(void) {
    for (uint8_t i = 0; i < MICROPY_PY_MACHINE_SPI_NUM_ENTRIES; i++) {
        if (machine_hw_spi_obj[i] == NULL) {
            machine_hw_spi_obj[i] = mp_obj_malloc(machine_spi_obj_t, &machine_spi_type);
            machine_hw_spi_obj[i]->div_num = SPI_CLK_DIV_BASE + i;
            return machine_hw_spi_obj[i];
        }
    }
    return NULL;
}

static inline void machine_hw_spi_obj_free(machine_spi_obj_t *spi_obj) {
    for (uint8_t i = 0; i < MICROPY_PY_MACHINE_SPI_NUM_ENTRIES; i++) {
        if (machine_hw_spi_obj[i] == spi_obj) {
            machine_hw_spi_obj[i] = NULL;
        }
    }
}

static void machine_spi_scb_isr(mp_obj_t hw_spi_obj) {
    machine_spi_obj_t *self = MP_OBJ_TO_PTR(hw_spi_obj);
    Cy_SCB_SPI_Interrupt(self->scb_obj->scb, &self->ctx);
}

static void machine_spi_hw_init(machine_spi_obj_t *self) {
    // 1. Validate
    if ((self->polarity > 1U) || (self->phase > 1U)) {
        mp_raise_ValueError(MP_ERROR_TEXT("polarity/phase must be 0 or 1"));
    }
    if (self->bits != 8U) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits must be 8"));
    }
    if ((self->firstbit != MICROPY_PY_MACHINE_SPI_MSB) &&
        (self->firstbit != MICROPY_PY_MACHINE_SPI_LSB)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid firstbit"));
    }
    if (self->is_slave && !self->has_ssel) {
        mp_raise_ValueError(MP_ERROR_TEXT("slave mode requires ssel pin"));
    }

    // 2. Pin config → discovers SCB unit
    uint8_t scb_unit = 0;
    if (self->has_ssel) {
        const mp_hal_pin_af_config_t spi_pins_config[] = {
            MP_HAL_PIN_AF_CONF(self->sck,
                self->is_slave ? CY_GPIO_DM_HIGHZ : CY_GPIO_DM_STRONG_IN_OFF,
                1, MACHINE_PIN_AF_SIGNAL_SPI_CLK),
            MP_HAL_PIN_AF_CONF(self->mosi,
                self->is_slave ? CY_GPIO_DM_HIGHZ : CY_GPIO_DM_STRONG_IN_OFF,
                1, MACHINE_PIN_AF_SIGNAL_SPI_MOSI),
            MP_HAL_PIN_AF_CONF(self->miso,
                self->is_slave ? CY_GPIO_DM_STRONG_IN_OFF : CY_GPIO_DM_HIGHZ,
                0, MACHINE_PIN_AF_SIGNAL_SPI_MISO),
            MP_HAL_PIN_AF_CONF(self->ssel,
                CY_GPIO_DM_HIGHZ, 0,
                MACHINE_PIN_AF_SIGNAL_SPI_SELECT0),
        };
        scb_unit = spi_pins_config[0].af->unit;
        mp_hal_periph_pins_af_config(spi_pins_config, 4);
    } else {
        const mp_hal_pin_af_config_t spi_pins_config[] = {
            MP_HAL_PIN_AF_CONF(self->sck,
                CY_GPIO_DM_STRONG_IN_OFF, 1,
                MACHINE_PIN_AF_SIGNAL_SPI_CLK),
            MP_HAL_PIN_AF_CONF(self->mosi,
                CY_GPIO_DM_STRONG_IN_OFF, 1,
                MACHINE_PIN_AF_SIGNAL_SPI_MOSI),
            MP_HAL_PIN_AF_CONF(self->miso,
                CY_GPIO_DM_HIGHZ, 0,
                MACHINE_PIN_AF_SIGNAL_SPI_MISO),
        };
        scb_unit = spi_pins_config[0].af->unit;
        mp_hal_periph_pins_af_config(spi_pins_config, 3);
    }

    // 3. Power on peripheral group
    const machine_scb_group_info_t *gi = &scb_group_table[scb_unit];
    Cy_SysClk_PeriGroupSlaveInit(gi->peri_nr, gi->group_nr,
        gi->slave_nr, gi->clk_hf_nr);

    // 4. Allocate SCB
    self->scb_obj = machine_scb_obj_alloc(scb_unit, self,
        machine_spi_scb_isr);

    // 5. Clock divider (master only)
    uint32_t actual_clk_scb = 0;
    if (!self->is_slave) {
        Cy_SysClk_PeriPclkDisableDivider(self->scb_obj->clk,
            SPI_CLK_DIV_TYPE, self->div_num);
        Cy_SysClk_PeriPclkAssignDivider(self->scb_obj->clk,
            SPI_CLK_DIV_TYPE, self->div_num);
        Cy_SysClk_PeriPclkSetDivider(self->scb_obj->clk,
            SPI_CLK_DIV_TYPE, self->div_num, 0U);
        Cy_SysClk_PeriPclkEnableDivider(self->scb_obj->clk,
            SPI_CLK_DIV_TYPE, self->div_num);

        uint32_t clk_hf_freq = Cy_SysClk_PeriPclkGetFrequency(
            self->scb_obj->clk, SPI_CLK_DIV_TYPE, self->div_num);

        uint32_t div_val = clk_hf_freq / (self->baudrate * SPI_OVERSAMPLE);
        if (div_val > 0U) {
            div_val--;
        }

        Cy_SysClk_PeriPclkDisableDivider(self->scb_obj->clk,
            SPI_CLK_DIV_TYPE, self->div_num);
        Cy_SysClk_PeriPclkSetDivider(self->scb_obj->clk,
            SPI_CLK_DIV_TYPE, self->div_num, div_val);
        Cy_SysClk_PeriPclkEnableDivider(self->scb_obj->clk,
            SPI_CLK_DIV_TYPE, self->div_num);

        actual_clk_scb = Cy_SysClk_PeriPclkGetFrequency(
            self->scb_obj->clk, SPI_CLK_DIV_TYPE, self->div_num);
        mplogger_print("SPI clk_hf=%u, div=%u, clk_scb=%u, baud=%u\n",
            clk_hf_freq, div_val, actual_clk_scb,
            actual_clk_scb / SPI_OVERSAMPLE);
    }

    // 6. PDL config struct
    cy_en_scb_spi_sclk_mode_t sclk_mode;
    if (self->polarity == 0 && self->phase == 0) {
        sclk_mode = CY_SCB_SPI_CPHA0_CPOL0;
    } else if (self->polarity == 0 && self->phase == 1) {
        sclk_mode = CY_SCB_SPI_CPHA1_CPOL0;
    } else if (self->polarity == 1 && self->phase == 0) {
        sclk_mode = CY_SCB_SPI_CPHA0_CPOL1;
    } else {
        sclk_mode = CY_SCB_SPI_CPHA1_CPOL1;
    }

    self->cfg = (cy_stc_scb_spi_config_t) {
        .spiMode = self->is_slave ? CY_SCB_SPI_SLAVE : CY_SCB_SPI_MASTER,
        .subMode = CY_SCB_SPI_MOTOROLA,
        .sclkMode = sclk_mode,
        .parity = CY_SCB_SPI_PARITY_NONE,
        .dropOnParityError = false,
        .oversample = self->is_slave ? 0U : SPI_OVERSAMPLE,
        .rxDataWidth = self->bits,
        .txDataWidth = self->bits,
        .enableMsbFirst = (self->firstbit == MICROPY_PY_MACHINE_SPI_MSB),
        .enableInputFilter = false,
        .enableFreeRunSclk = false,
        .enableMisoLateSample = self->is_slave ? false : true,
        .enableTransferSeparation = false,
        .ssPolarity =
            ((CY_SCB_SPI_ACTIVE_LOW << CY_SCB_SPI_SLAVE_SELECT0) |
                (CY_SCB_SPI_ACTIVE_LOW << CY_SCB_SPI_SLAVE_SELECT1) |
                (CY_SCB_SPI_ACTIVE_LOW << CY_SCB_SPI_SLAVE_SELECT2) |
                (CY_SCB_SPI_ACTIVE_LOW << CY_SCB_SPI_SLAVE_SELECT3)),
        .ssSetupDelay = false,
        .ssHoldDelay = false,
        .ssInterFrameDelay = false,
        .enableWakeFromSleep = false,
        .rxFifoTriggerLevel = 0UL,
        .rxFifoIntEnableMask = 0UL,
        .txFifoTriggerLevel = 0UL,
        .txFifoIntEnableMask = 0UL,
        .masterSlaveIntEnableMask = 0UL,
    };

    // 7. Init + enable
    cy_en_scb_spi_status_t result =
        Cy_SCB_SPI_Init(self->scb_obj->scb, &self->cfg, &self->ctx);
    if (result != CY_SCB_SPI_SUCCESS) {
        machine_scb_obj_free(self->scb_obj);
        self->scb_obj = NULL;
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("SPI init failed: 0x%lx"), (uint32_t)result);
    }

    sys_int_init(&self->scb_obj->irq);
    Cy_SCB_SPI_Enable(self->scb_obj->scb);
}

// Disables the peripheral, resets registers, frees the interrupt,
static void machine_spi_hw_deinit(machine_spi_obj_t *self) {
    Cy_SCB_SPI_Disable(self->scb_obj->scb, &self->ctx);
    Cy_SCB_SPI_DeInit(self->scb_obj->scb);
    sys_int_deinit(&self->scb_obj->irq);
    Cy_SysClk_PeriPclkDisableDivider(self->scb_obj->clk, SPI_CLK_DIV_TYPE, self->div_num);
    machine_scb_obj_free(self->scb_obj);
    self->scb_obj = NULL;
}

/* Port SPI protocol callbacks  */

static void machine_spi_init(mp_obj_base_t *self,
    size_t n_args,
    const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    // hardware-specific init to be implemented by port
}

static void machine_spi_deinit(mp_obj_base_t *self_in) {
    machine_spi_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->scb_obj != NULL) {
        machine_spi_hw_deinit(self);
    }
    machine_hw_spi_obj_free(self);
}

static void machine_spi_transfer(mp_obj_base_t *self,
    size_t len,
    const uint8_t *src,
    uint8_t *dest) {
    // hardware-specific transfer to be implemented by port
}

/* SPI protocol table  */

static const mp_machine_spi_p_t machine_spi_p = {
    .init = machine_spi_init,
    .deinit = machine_spi_deinit,
    .transfer = machine_spi_transfer,
};

/* SPI print  */

static void machine_spi_print(const mp_print_t *print,
    mp_obj_t self_in,
    mp_print_kind_t kind) {
    mp_printf(print, "SPI()");
}


mp_obj_t mp_machine_spi_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 11, true);

    // Argument definitions: sck/mosi/miso are required, rest have defaults
    enum { ARG_id, ARG_baudrate, ARG_polarity, ARG_phase, ARG_bits,
           ARG_firstbit, ARG_sck, ARG_mosi, ARG_miso, ARG_slave, ARG_ssel };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id,       MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_baudrate, MP_ARG_INT, {.u_int = DEFAULT_SPI_BAUDRATE} },
        { MP_QSTR_polarity, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = DEFAULT_SPI_POLARITY} },
        { MP_QSTR_phase,    MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = DEFAULT_SPI_PHASE} },
        { MP_QSTR_bits,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = DEFAULT_SPI_BITS} },
        { MP_QSTR_firstbit, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = MICROPY_PY_MACHINE_SPI_MSB} },
        { MP_QSTR_sck,      MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_mosi,     MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_miso,     MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_slave,    MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_ssel,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };

    // Parse positional and keyword arguments from Python call
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // Allocate from static pool (each slot has a unique clock divider index)
    machine_spi_obj_t *self = machine_hw_spi_obj_alloc();
    if (self == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("machine.SPI: Maximum number of SPI instances reached"));
    }

    // Store SPI parameters from parsed args
    self->baudrate = args[ARG_baudrate].u_int;
    self->polarity = args[ARG_polarity].u_int;
    self->phase = args[ARG_phase].u_int;
    self->bits = args[ARG_bits].u_int;
    self->firstbit = args[ARG_firstbit].u_int;
    self->is_slave = args[ARG_slave].u_bool;
    self->timeout = DEFAULT_SPI_TIMEOUT;
    self->scb_obj = NULL;

    // Resolve pin name strings (e.g. 'P5_0') to internal pin objects
    self->sck = mp_hal_get_pin_obj(args[ARG_sck].u_obj);
    self->mosi = mp_hal_get_pin_obj(args[ARG_mosi].u_obj);
    self->miso = mp_hal_get_pin_obj(args[ARG_miso].u_obj);
    self->has_ssel = args[ARG_ssel].u_obj != mp_const_none;
    if (self->has_ssel) {
        self->ssel = mp_hal_get_pin_obj(args[ARG_ssel].u_obj);
    }

    // Initialize hardware with try/except pattern (nlr = non-local return).
    // If hw_init raises, free the pool slot before re-raising to Python.
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        machine_spi_hw_init(self);
        nlr_pop();
    } else {
        machine_hw_spi_obj_free(self);
        nlr_jump(nlr.ret_val);
    }

    return MP_OBJ_FROM_PTR(self);
}

MP_DEFINE_CONST_OBJ_TYPE(
    machine_spi_type,
    MP_QSTR_SPI,
    MP_TYPE_FLAG_NONE,
    make_new, mp_machine_spi_make_new,
    print, machine_spi_print,
    protocol, &machine_spi_p,
    locals_dict, &mp_machine_spi_locals_dict
    );
