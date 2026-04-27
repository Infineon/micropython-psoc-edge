/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022-2026 Infineon Technologies AG
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

// Lookup table: maps SCB unit number → PeriGroupSlaveInit arguments.
// Populated at compile time via the board-generated X-macro.
typedef struct {
    uint32_t peri_nr;
    uint32_t group_nr;
    uint32_t slave_nr;
    uint32_t clk_hf_nr;
} machine_scb_group_info_t;

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

/* Port SPI protocol callbacks  */

static void machine_spi_init(mp_obj_base_t *self,
    size_t n_args,
    const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    // hardware-specific init to be implemented by port
}

static void machine_spi_deinit(mp_obj_base_t *self) {
    // hardware-specific deinit to be implemented by port
}

static void machine_spi_transfer(mp_obj_base_t *self,
    size_t len,
    const uint8_t *src,
    uint8_t *dest) {
    // hardware-specific transfer to be implemented by port
}

/* SPI protocol table — REQUIRED BY TEMPLATE */

static const mp_machine_spi_p_t machine_spi_p = {
    .init = machine_spi_init,
    .deinit = machine_spi_deinit,
    .transfer = machine_spi_transfer,
};

/* SPI object construction */

static mp_obj_t machine_spi_make_new(const mp_obj_type_t *type,
    size_t n_args,
    size_t n_kw,
    const mp_obj_t *all_args) {
    machine_spi_obj_t *self =
        mp_obj_malloc(machine_spi_obj_t, type);
    return MP_OBJ_FROM_PTR(self);
}

/* SPI print helper */

static void machine_spi_print(const mp_print_t *print,
    mp_obj_t self_in,
    mp_print_kind_t kind) {
    mp_printf(print, "SPI()");
}


MP_DEFINE_CONST_OBJ_TYPE(
    machine_spi_type,
    MP_QSTR_SPI,
    MP_TYPE_FLAG_NONE,
    make_new, machine_spi_make_new,
    print, machine_spi_print,
    protocol, &machine_spi_p,
    locals_dict, &mp_machine_spi_locals_dict
    );
