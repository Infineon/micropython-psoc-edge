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

// std includes
#include <stdio.h>
#include <string.h>


// mpy includes
#include "extmod/modmachine.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mperrno.h"


// MTB includes
#include "cybsp.h"
#include "cy_scb_i2c.h"
#include "cy_sysint.h"
#include "cy_sysclk.h"
#include "cy_gpio.h"


// port-specific includes
#include "modmachine.h"
#include "mplogger.h"


/*
 * Multi-Platform I2C Driver - ID-based Instance Selection
 *
 * This driver supports multiple I2C instances using ID-based selection.
 * Pin configuration is board-independent and defined in mpconfigboard.h.
 *
 * Usage:
 *   i2c0 = I2C(0)  # Use I2C instance 0 (pins from board config)
 *   i2c1 = I2C(1)  # Use I2C instance 1 (pins from board config)
 *
 * Note: Pin parameters are not supported - instances use board-defined pins.
 * Board configuration defines pin numbers and hardware mapping.
 */
#define DEFAULT_I2C_FREQ     (400000)

#define i2c_assert_raise_val(msg, ret)   if (ret != CY_RSLT_SUCCESS) { \
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT(msg), ret); \
}

// PSoC Edge I2C hardware configuration structure (matches board config list)
typedef struct {
    // i2c instance hardware parameters
    int id;                        // I2C instance ID
    CySCB_Type *scb;               // SCB block pointer
    en_clk_dst_t pclk;             // Peripheral clock
    IRQn_Type irqn;                // Interrupt number
    GPIO_PRT_Type *gpio_port;      // GPIO port

    // Pin configuration parameters
    // TODO: Replace after improvements of machine.pin. This can be resolved using machine.Pin
    uint32_t scl_pin_num;          // SCL pin number (P17_0_NUM = 0)
    uint32_t sda_pin_num;          // SDA pin number (P17_1_NUM = 1)
    uint32_t scl_hsiom;            // SCL HSIOM value
    uint32_t sda_hsiom;            // SDA HSIOM value
    bool supports_target;          // Whether this instance supports target mode
} psoc_edge_i2c_hw_config_t;

typedef struct _machine_hw_i2c_obj_t {
    mp_obj_base_t base;
    int id;                        // I2C instance ID (0, 1, ...)
    uint32_t freq;
    uint32_t timeout;

    // Platform-specific hardware interface (can be abstracted further)
    CySCB_Type *scb;               // SCB Block (PSoC specific)
    uint32_t pclk;                 // Peripheral clock
    IRQn_Type irqn;                // Interrupt number
    cy_stc_scb_i2c_config_t cfg;   // PDL I2C configuration
    cy_stc_scb_i2c_context_t ctx;  // PDL I2C runtime context
} machine_hw_i2c_obj_t;

// PSoC Edge I2C hardware configurations (from mpconfigboard.h)
// Global visibility for machine_i2c_target.c access
const psoc_edge_i2c_hw_config_t psoc_edge_i2c_hw_configs[] = {
    MICROPY_HW_I2C_CONFIG_LIST
};

machine_hw_i2c_obj_t *machine_hw_i2c_obj[MICROPY_HW_MAX_I2C] = { NULL };


static int machine_hw_i2c_deinit(mp_obj_base_t *self_in);

// Get PSoC Edge I2C hardware configuration by ID
static const psoc_edge_i2c_hw_config_t *i2c_get_hw_config(int i2c_id) {
    for (int i = 0; i < MICROPY_HW_MAX_I2C; i++) {
        if (psoc_edge_i2c_hw_configs[i].id == i2c_id) {
            return &psoc_edge_i2c_hw_configs[i];
        }
    }
    return NULL;
}

// I2C instance validation function
static int i2c_find_instance_by_id(int i2c_id) {
    return (i2c_get_hw_config(i2c_id) != NULL) ? i2c_id : -1;
}

// I2C interrupt service routine
static void machine_i2c_isr(void) {
    for (uint8_t i = 0; i < MICROPY_HW_MAX_I2C; i++) {
        if (machine_hw_i2c_obj[i] != NULL) {
            Cy_SCB_I2C_MasterInterrupt(machine_hw_i2c_obj[i]->scb, &machine_hw_i2c_obj[i]->ctx);
        }
    }
}

// Allocate I2C object for specific ID (direct allocation)
static inline machine_hw_i2c_obj_t *machine_hw_i2c_obj_alloc_for_id(int i2c_id) {
    // First validate the ID
    if (i2c_id < 0 || i2c_id >= MICROPY_HW_MAX_I2C) {
        return NULL;
    }

    // Check if this ID slot is already occupied
    if (machine_hw_i2c_obj[i2c_id] != NULL) {
        // Deinitialize existing instance for reinitialization
        mplogger_print("Reinitializing existing I2C%d instance\n", i2c_id);
        machine_hw_i2c_deinit((mp_obj_base_t *)machine_hw_i2c_obj[i2c_id]);
        machine_hw_i2c_obj[i2c_id] = NULL;
    }

    // Allocate new object directly in the target slot
    machine_hw_i2c_obj[i2c_id] = mp_obj_malloc(machine_hw_i2c_obj_t, &machine_i2c_type);
    return machine_hw_i2c_obj[i2c_id];
}

static inline void machine_hw_i2c_obj_free(machine_hw_i2c_obj_t *i2c_obj_ptr) {
    for (uint8_t i = 0; i < MICROPY_HW_MAX_I2C; i++)
    {
        if (machine_hw_i2c_obj[i] == i2c_obj_ptr) {
            machine_hw_i2c_obj[i] = NULL;
        }
    }
}

static void machine_hw_i2c_init(machine_hw_i2c_obj_t *self, uint32_t freq_hz) {
    cy_rslt_t result;

    // Get PSoC Edge hardware configuration for this I2C ID
    const psoc_edge_i2c_hw_config_t *hw_config = i2c_get_hw_config(self->id);

    if (hw_config == NULL) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("I2C ID %d not available"), self->id);
    }

    // Store hardware parameters from hw config
    self->scb = hw_config->scb;
    self->pclk = hw_config->pclk;
    self->irqn = hw_config->irqn;

    self->cfg = (cy_stc_scb_i2c_config_t) {
        .i2cMode = CY_SCB_I2C_MASTER,
        .useRxFifo = false,
        .useTxFifo = true,
        .slaveAddress = 0U,
        .slaveAddressMask = 0U,
        .acceptAddrInFifo = false,
        .ackGeneralAddr = false,
        .enableWakeFromSleep = false,
        .enableDigitalFilter = false,
        .lowPhaseDutyCycle = 8U,
        .highPhaseDutyCycle = 8U,
    };

    //  TODO: Replaced after machine.pin
    // GPIO configuration using hardware configuration
    Cy_GPIO_SetHSIOM(hw_config->gpio_port, hw_config->scl_pin_num, hw_config->scl_hsiom);
    Cy_GPIO_SetHSIOM(hw_config->gpio_port, hw_config->sda_pin_num, hw_config->sda_hsiom);
    Cy_GPIO_SetDrivemode(hw_config->gpio_port, hw_config->scl_pin_num, MICROPY_HW_I2C_GPIO_DRIVE_MODE);
    Cy_GPIO_SetDrivemode(hw_config->gpio_port, hw_config->sda_pin_num, MICROPY_HW_I2C_GPIO_DRIVE_MODE);

    result = Cy_SCB_I2C_Init(self->scb, &self->cfg, &self->ctx);
    if (result != CY_RSLT_SUCCESS) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("I2C init failed: 0x%lx"), result);
    }

    // For desired data rate, clk_scb frequency must be in valid range (see TRM I2C Oversampling section)
    // For 100kHz: clk_scb range is 1.55 - 3.2 MHz (architecture reference manual 002-38331 Rev. P685 table 355)
    //   - clk_peri = 100 MHz, divider = 42 → clk_scb = 2.38 MHz ✓ (mid-range)
    // For 400kHz: clk_scb range is 7.82 - 10 MHz
    //   - clk_peri = 100 MHz, divider = 11 → clk_scb = 9.09 MHz ✓ (within range)
    // Note: Cy_SysClk_PeriphSetDivider takes (divider - 1), so divider=11 → value=10

    /* Connect assigned divider to be a clock source for I2C */
    Cy_SysClk_PeriphAssignDivider(self->pclk, CY_SYSCLK_DIV_8_BIT, 2U);
    uint32_t divider = (freq_hz <= 100000) ? 41U : 10U;
    Cy_SysClk_PeriphSetDivider(CY_SYSCLK_DIV_8_BIT, 2U, divider);
    Cy_SysClk_PeriphEnableDivider(CY_SYSCLK_DIV_8_BIT, 2U);

    uint32_t clk_scb_freq = Cy_SysClk_PeriphGetFrequency(CY_SYSCLK_DIV_8_BIT, 2U);
    mplogger_print("DEBUG: I2C%d clk_scb_freq=%lu Hz\n", self->id, clk_scb_freq);

    uint32_t actual_rate = Cy_SCB_I2C_SetDataRate(self->scb, freq_hz, clk_scb_freq);
    mplogger_print("DEBUG: I2C%d actual_rate=%lu Hz (requested=%lu Hz)\n", self->id, actual_rate, freq_hz);

    if ((actual_rate > freq_hz) || (actual_rate == 0U)) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("Cannot reach desired I2C data rate %u Hz (actual: %u Hz)"),
            freq_hz, actual_rate);
    }

    const cy_stc_sysint_t i2cIntrConfig = {
        .intrSrc = self->irqn,
        .intrPriority = MICROPY_HW_I2C_INTR_PRIORITY,
    };

    // Hook interrupt service routine and enable interrupt in NVIC
    result = Cy_SysInt_Init(&i2cIntrConfig, &machine_i2c_isr);
    if (result != CY_RSLT_SUCCESS) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("I2C interrupt init failed: 0x%lx"), result);
    }
    NVIC_EnableIRQ(self->irqn);

    Cy_SCB_I2C_Enable(self->scb);

    mplogger_print("I2C%d initialized: requested=%lu Hz, actual=%lu Hz, clk_scb=%lu Hz\n",
        self->id, freq_hz, actual_rate, clk_scb_freq);

    self->freq = freq_hz;
}


static int machine_hw_i2c_deinit(mp_obj_base_t *self_in) {
    machine_hw_i2c_obj_t *self = MP_OBJ_TO_PTR(self_in);

    Cy_SCB_I2C_Disable(self->scb, &self->ctx);
    NVIC_DisableIRQ(self->irqn);
    Cy_SysClk_PeriphDisableDivider(CY_SYSCLK_DIV_8_BIT, 0U);

    // Clear the instance slot
    machine_hw_i2c_obj[self->id] = NULL;

    mplogger_print("I2C%d deinitialized\n", self->id);

    return 0;  // Success
}

static int machine_hw_i2c_transfer(mp_obj_base_t *self_in, uint16_t addr, size_t len, uint8_t *buf, unsigned int flags) {
    machine_hw_i2c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    cy_rslt_t result;

    mplogger_print("I2C Transfer: addr=0x%02X, len=%zu, flags=0x%02X (%s)\n",
        addr, len, flags, (flags & MP_MACHINE_I2C_FLAG_READ) ? "READ" : "WRITE");

    cy_stc_scb_i2c_master_xfer_config_t transfer;
    transfer.slaveAddress = addr;
    transfer.buffer = buf;
    transfer.bufferSize = len;
    // Generate Stop condition if MP_MACHINE_I2C_FLAG_STOP is set
    transfer.xferPending = !(flags & MP_MACHINE_I2C_FLAG_STOP);

    if (flags & MP_MACHINE_I2C_FLAG_READ) {
        result = Cy_SCB_I2C_MasterRead(self->scb, &transfer, &self->ctx);
    } else {
        result = Cy_SCB_I2C_MasterWrite(self->scb, &transfer, &self->ctx);
    }

    if (result != CY_RSLT_SUCCESS) {
        mplogger_print("I2C Transfer start failed: 0x%lx\n", result);
        return -MP_EIO;  // I/O error
    }

    mplogger_print("I2C Transfer started, waiting for completion...\n");

    uint32_t start_time = mp_hal_ticks_us();
    uint32_t timeout_end = start_time + self->timeout;  // Both in microseconds

    while (0UL != (CY_SCB_I2C_MASTER_BUSY & Cy_SCB_I2C_MasterGetStatus(self->scb, &self->ctx))) {
        // Yield to allow other tasks/interrupts to run
        MICROPY_EVENT_POLL_HOOK

        // Check for timeout using actual elapsed time
        if (mp_hal_ticks_us() >= timeout_end) {
            mplogger_print("I2C Transfer timeout after %u us!\n", self->timeout);
            return -MP_ETIMEDOUT;
        }
    }

    uint32_t master_status = Cy_SCB_I2C_MasterGetStatus(self->scb, &self->ctx);

    mplogger_print("I2C Transfer complete, status=0x%08lX\n", master_status);

    if (master_status & CY_SCB_I2C_MASTER_ERR) {
        mplogger_print("I2C Transfer error detected in status\n");
        return -MP_EIO;  // I/O error
    }

    return len;
}

/******************************************************************************/
// MicroPython bindings for machine API

static void machine_hw_i2c_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_hw_i2c_obj_t *self = MP_OBJ_TO_PTR(self_in);

    // Print I2C configuration
    mp_printf(print, "I2C(id=%u, freq=%u, timeout=%u)",
        self->id,
        self->freq,
        self->timeout);
}

mp_obj_t machine_hw_i2c_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 3, true);

    enum { ARG_id, ARG_freq, ARG_timeout };

    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id,   MP_ARG_INT, {.u_int = 0}},  // Default to I2C0
        { MP_QSTR_freq, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = DEFAULT_I2C_FREQ} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 50000} }  // Default 50000us (50ms)
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // Determine which I2C instance to use (ID-based selection only)
    int i2c_id = args[ARG_id].u_int;

    // Validate I2C ID
    if (i2c_find_instance_by_id(i2c_id) == -1) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid I2C ID"));
    }

    // Allocate I2C object directly for the target ID
    machine_hw_i2c_obj_t *self = machine_hw_i2c_obj_alloc_for_id(i2c_id);
    if (self == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("Failed to allocate I2C instance"));
    }

    // Set pins from hardware configuration (board-independent)
    // Pin configuration is handled by GPIO macros in init function

    self->id = i2c_id;
    self->freq = args[ARG_freq].u_int;
    self->timeout = args[ARG_timeout].u_int;

    if (self->timeout == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout must be > 0"));
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        machine_hw_i2c_init(self, self->freq);
        nlr_pop();
    } else {
        // Initialization failed, clean up
        machine_hw_i2c_obj[i2c_id] = NULL;
        nlr_jump(nlr.ret_val);
    }

    mplogger_print("I2C%d created successfully\n", i2c_id);

    return MP_OBJ_FROM_PTR(self);
}

static const mp_machine_i2c_p_t machine_hw_i2c_p = {
    .transfer = mp_machine_i2c_transfer_adaptor,
    .transfer_single = machine_hw_i2c_transfer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    machine_i2c_type,
    MP_QSTR_I2C,
    MP_TYPE_FLAG_NONE,
    make_new, machine_hw_i2c_make_new,
    print, machine_hw_i2c_print,
    protocol, &machine_hw_i2c_p,
    locals_dict, &mp_machine_i2c_locals_dict
    );
