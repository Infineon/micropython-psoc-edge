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

// MTB includes
#include "cybsp.h"
#include "cy_scb_i2c.h"
#include "cy_sysint.h"
#include "cy_sysclk.h"

// MicroPython includes
#include "py/runtime.h"
#include "py/mphal.h"
#include "extmod/modmachine.h"

// port-specific
#include "clk.h"
#include "genhdr/pins_af.h"
#include "modmachine.h"
// #include "scb.h"
#include "i2c.h"

#define DEBUG_printf(...) // printf(__VA_ARGS__)

typedef struct _machine_i2c_target_obj_t {
    mp_obj_base_t base;
    mp_hal_pin_obj_t scl;
    mp_hal_pin_obj_t sda;
    uint32_t addr;
    uint8_t addrsize;
    mp_obj_t mem;
    size_t mem_addrsize;
    size_t tx_index;
    size_t rx_index;
    i2c_obj_t *i2c_obj;
} machine_i2c_target_obj_t;

static machine_i2c_target_obj_t *machine_i2c_target_obj[MICROPY_PY_MACHINE_I2C_NUM_ENTRIES] = {NULL};

static machine_i2c_target_obj_t *machine_i2c_target_obj_get(uint8_t id);
static void i2c_target_obj_event_callback(machine_i2c_target_obj_t *self, uint32_t events);

#define DEFINE_I2C_TARGET_EVENT_CALLBACK(scb) \
    void i2c_##scb##_target_event_callback(uint32_t events) { \
        machine_i2c_target_obj_t *self = machine_i2c_target_obj_get(scb); \
        i2c_target_obj_event_callback(self, events); \
    }

/**
 * The file build-<board>/genhdr/pins_af.h contains the macro
 *
 *  MICROPY_PY_FOR_ALL_SCB(DO)
 *
 *  which uses the X-macro (as argument) pattern to pass a worker
 *  macro DO(port) for the list of all user available ports.
 *
 * The available (not hidden) user SCBs are those alternate
 * functions defined in the boards/pse8x_af.csv file, for which
 * the corresponding pin are available for the user.
 * The available pins are those defined in the
 * boards/<board>/pins.csv file, which are not prefixed
 * with a hyphen(-).
 * See tools/boardgen.py and psoc-edge/boards/make-pins.py
 * for more information.
 */

MICROPY_PY_FOR_ALL_SCB(DEFINE_I2C_TARGET_EVENT_CALLBACK)

#define I2C_TARGET_EVENT_CALLBACK_ENTRY(scb) \
    [scb] = i2c_##scb##_target_event_callback,

static cy_cb_scb_i2c_handle_events_t i2c_target_event_callback[MICROPY_PY_SCB_NUM_ENTRIES] = {
    MICROPY_PY_FOR_ALL_SCB(I2C_TARGET_EVENT_CALLBACK_ENTRY)
};


/******************************************************************************/
// PSOC PDL hardware bindings

// PDL event callback - called from within Cy_SCB_I2C_SlaveInterrupt
// Implements the event handling pattern from PDL Slave Operation documentation
//
// Key PDL requirements implemented:
// 1. Register callback during init: Cy_SCB_I2C_RegisterEventCallback()
// 2. Handle completion events: RD_CMPLT_EVENT, WR_CMPLT_EVENT
// 3. Reconfigure buffers after each transaction (critical!)
// 4. Clear status flags after write: Cy_SCB_I2C_SlaveClearWriteStatus()
//
// Note: Without buffer reconfiguration, next transaction continues from
// where previous stopped (e.g., if master read 8 of 10 bytes, next read
// starts at byte 9). This is PDL documented behavior.
static void i2c_target_obj_event_callback(machine_i2c_target_obj_t *self, uint32_t events) {
    if (self == NULL) {
        return;
    }

    machine_i2c_target_data_t *data = &machine_i2c_target_data[i2c_get_id(self->i2c_obj)];
    // I2CTarget.IRQ_ADDR_MATCH_READ: master sent address with read bit.
    if (events & CY_SCB_I2C_SLAVE_READ_EVENT) {
        machine_i2c_target_data_addr_match(data, true);
    }


    // I2CTarget.IRQ_ADDR_MATCH_WRITE: master sent address with write bit.
    if (events & CY_SCB_I2C_SLAVE_WRITE_EVENT) {
        machine_i2c_target_data_addr_match(data, false);
    }


    if (events & CY_SCB_I2C_SLAVE_RD_BUF_EMPTY_EVENT) {
        // I2CTarget.IRQ_READ_REQ: TX buffer consumed and another byte requested by master.
        if (data->mem_buf != NULL && data->mem_len > 0) {
            machine_i2c_target_data_read_request(self, data);
        }
    }

    if (events & CY_SCB_I2C_SLAVE_RD_CMPLT_EVENT) {
        if (data->mem_buf != NULL && data->mem_len > 0) {
            i2c_hw_target_config_read_buff(self->i2c_obj, data->mem_buf, data->mem_len);
        }

        i2c_hw_target_clear_read_status(self->i2c_obj);

        // Reset index for next transaction
        self->tx_index = 0;

        // Set state to READING so extmod reset_helper triggers END_READ
        data->state = STATE_READING;

        // I2CTarget.IRQ_END_READ: read transaction completed (STOP/restart observed).
        machine_i2c_target_data_restart_or_stop(data);
    }

    if (events & CY_SCB_I2C_SLAVE_WR_CMPLT_EVENT) {
        if (!(events & CY_SCB_I2C_SLAVE_ERR_EVENT)) {
            uint32_t bytes_received = i2c_hw_target_get_write_count(self->i2c_obj);
            self->rx_index = 0;
            while (self->rx_index < bytes_received) {
                // I2CTarget.IRQ_WRITE_REQ: incoming write payload available from master.
                machine_i2c_target_data_write_request(self, data);
            }
        }

        if (data->mem_buf != NULL && data->mem_len > 0) {
            i2c_hw_target_config_write_buff(self->i2c_obj, data->mem_buf, data->mem_len);
        }

        i2c_hw_target_clear_write_status(self->i2c_obj);

        // Ensure state is WRITING so extmod reset_helper triggers END_WRITE
        data->state = STATE_WRITING;

        // I2CTarget.IRQ_END_WRITE: write transaction completed (STOP/restart observed).
        machine_i2c_target_data_restart_or_stop(data);
    }

    // Handle errors
    if (events & CY_SCB_I2C_SLAVE_ERR_EVENT) {
        machine_i2c_target_data_restart_or_stop(data);
    }
}

static void machine_i2c_target_scb_isr(mp_obj_t i2c_target_obj) {
    machine_i2c_target_obj_t *self = MP_OBJ_TO_PTR(i2c_target_obj);
    i2c_hw_target_irq(self->i2c_obj);
}

static machine_i2c_target_obj_t *machine_i2c_target_obj_get(uint8_t id) {
    for (uint8_t i = 0; i < MICROPY_PY_MACHINE_I2C_NUM_ENTRIES; i++) {
        if (machine_i2c_target_obj[i] != NULL) {
            if (i2c_get_id(machine_i2c_target_obj[i]->i2c_obj) == id) {
                return machine_i2c_target_obj[i];
            }
        }
    }
    return NULL;
}

static inline machine_i2c_target_obj_t *machine_i2c_target_obj_alloc(void) {
    for (uint8_t i = 0; i < MICROPY_PY_MACHINE_I2C_NUM_ENTRIES; i++)
    {
        if (machine_i2c_target_obj[i] == NULL) {
            machine_i2c_target_obj[i] = mp_obj_malloc(machine_i2c_target_obj_t, &machine_i2c_target_type);
            return machine_i2c_target_obj[i];
        }
    }
    return NULL;
}

static inline void machine_i2c_target_obj_free(machine_i2c_target_obj_t *i2c_target_obj_ptr) {
    for (uint8_t i = 0; i < MICROPY_PY_MACHINE_I2C_NUM_ENTRIES; i++)
    {
        if (machine_i2c_target_obj[i] == i2c_target_obj_ptr) {
            machine_i2c_target_obj[i] = NULL;
        }
    }
}


static void machine_i2c_target_hw_init(machine_i2c_target_obj_t *self) {
    i2c_hw_target_init(self->i2c_obj, self->addr);

    i2c_hw_target_register_callback(self->i2c_obj, i2c_target_event_callback[i2c_get_id(self->i2c_obj)]);

    MP_STATE_PORT(machine_i2c_target_mem_obj)[i2c_get_id(self->i2c_obj)] = self->mem;
    machine_i2c_target_data_init(&machine_i2c_target_data[i2c_get_id(self->i2c_obj)], self->mem, self->mem_addrsize);

    machine_i2c_target_data_t *data = &machine_i2c_target_data[i2c_get_id(self->i2c_obj)];
    if (data->mem_buf != NULL && data->mem_len > 0) {
        i2c_hw_target_config_read_buff(self->i2c_obj, data->mem_buf, data->mem_len);
        i2c_hw_target_config_write_buff(self->i2c_obj, data->mem_buf, data->mem_len);
    }

    DEBUG_printf("I2C Target initialized: addr=0x%02X, addrsize=%u-bit\n", self->addr, self->addrsize);
}

/******************************************************************************/
// I2CTarget port implementation

static inline size_t mp_machine_i2c_target_get_index(machine_i2c_target_obj_t *self) {
    return i2c_get_id(self->i2c_obj);
}

// IRQ event callback - called from extmod to trigger Python IRQ handler
// This is called by handle_event() in extmod/machine_i2c_target.c
static void mp_machine_i2c_target_event_callback(machine_i2c_target_irq_obj_t *irq) {
    if (irq->base.handler != mp_const_none) {
        mp_irq_handler(&irq->base);
    }
}

static size_t mp_machine_i2c_target_read_bytes(machine_i2c_target_obj_t *self, size_t len, uint8_t *buf) {
    machine_i2c_target_data_t *data = &machine_i2c_target_data[i2c_get_id(self->i2c_obj)];
    size_t read_len = 0;

    i2c_hw_target_irq_disable(self->i2c_obj);

    uint32_t available = i2c_hw_target_get_write_count(self->i2c_obj);
    read_len = (len < available) ? len : available;

    if (data->mem_buf != NULL) {
        for (size_t i = 0; i < read_len; i++) {
            if (self->rx_index < data->mem_len) {
                buf[i] = data->mem_buf[self->rx_index++];
            }
        }
    }

    i2c_hw_target_irq_enable(self->i2c_obj);

    return read_len;
}

static size_t mp_machine_i2c_target_write_bytes(machine_i2c_target_obj_t *self, size_t len, const uint8_t *buf) {
    machine_i2c_target_data_t *data = &machine_i2c_target_data[i2c_get_id(self->i2c_obj)];
    size_t write_len = 0;

    i2c_hw_target_irq_disable(self->i2c_obj);

    if (data->mem_buf != NULL) {
        for (size_t i = 0; i < len; i++) {
            if (self->tx_index < data->mem_len) {
                data->mem_buf[self->tx_index++] = buf[i];
                write_len++;
            }
        }

        // Update slave read buffer to reflect new data (per PDL documentation)
        i2c_hw_target_config_read_buff(self->i2c_obj, data->mem_buf, self->tx_index);
    }

    i2c_hw_target_irq_enable(self->i2c_obj);

    return write_len;
}

static void mp_machine_i2c_target_irq_config(machine_i2c_target_obj_t *self, unsigned int trigger) {
    // IRQ configuration already handled in init
}

enum { ARG_id, ARG_addr, ARG_addrsize, ARG_mem, ARG_mem_addrsize, ARG_scl, ARG_sda };
static const mp_arg_t allowed_args[] = {
    { MP_QSTR_id, MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_addr, MP_ARG_REQUIRED | MP_ARG_INT },
    { MP_QSTR_addrsize, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 7} },
    { MP_QSTR_mem, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_mem_addrsize, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_scl, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_sda, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
};

static void machine_i2c_target_init_impl(machine_i2c_target_obj_t *self, int i2c_id, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_hal_pin_af_config_t i2c_pins_af_config[] = {
        MP_HAL_PIN_AF_CONF_INIT_GPIO_SIGNAL(CY_GPIO_DM_OD_DRIVESLOW, 1, MACHINE_PIN_AF_SIGNAL_I2C_SCL),
        MP_HAL_PIN_AF_CONF_INIT_GPIO_SIGNAL(CY_GPIO_DM_OD_DRIVESLOW, 1, MACHINE_PIN_AF_SIGNAL_I2C_SDA),
    };

    if (args[ARG_scl].u_obj != mp_const_none) {
        mp_hal_pin_obj_t scl_pin = mp_hal_get_pin_obj(args[ARG_scl].u_obj);
        MP_HAL_PIN_AF_CONF_SET_PIN_AF(i2c_pins_af_config[0], scl_pin);
    }

    if (args[ARG_sda].u_obj != mp_const_none) {
        mp_hal_pin_obj_t sda_pin = mp_hal_get_pin_obj(args[ARG_sda].u_obj);
        MP_HAL_PIN_AF_CONF_SET_PIN_AF(i2c_pins_af_config[1], sda_pin);
    }

    /* -- Resolve ID - pin match -- */
    machine_pin_af_unit_t fn_unit = (machine_pin_af_unit_t)i2c_id;
    mp_hal_periph_pins_af_resolve_fn_unit(i2c_pins_af_config, 2, MACHINE_PIN_AF_FN_I2C, &fn_unit);

    /* -- Resolve not provided AF pins -- */
    mp_hal_periph_pins_af_resolve_pin_af(i2c_pins_af_config, 2, fn_unit);

    /** -- Target address -- **/
    if (args[ARG_addrsize].u_int != 7 && args[ARG_addrsize].u_int != 10) {
        mp_raise_ValueError(MP_ERROR_TEXT("addrsize must be 7 or 10"));
    }
    uint8_t addrsize = args[ARG_addrsize].u_int;

    /** -- Memory address size -- **/
    if (args[ARG_mem_addrsize].u_int != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("mem_addrsize must be 0 (EEPROM-like addressing not implemented)"));
    }
    uint32_t mem_addrsize = args[ARG_mem_addrsize].u_int;

    /* -- Object allocation -- */
    bool is_new = false;
    self->i2c_obj = i2c_alloc(fn_unit, &is_new, self, machine_i2c_target_scb_isr);

    /* -- Reinitialization reset -- */
    if (!is_new) {
        i2c_hw_deinit(self->i2c_obj);
    }

    /* -- I2C params init -- */
    self->scl = i2c_pins_af_config[0].pin;
    self->sda = i2c_pins_af_config[1].pin;
    self->mem = args[ARG_mem].u_obj;
    self->addr = args[ARG_addr].u_int;
    self->addrsize = addrsize;
    self->mem_addrsize = mem_addrsize;
    self->tx_index = 0;
    self->rx_index = 0;

    /* -- Initialise hardware -- */
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_hal_periph_pins_af_init(i2c_pins_af_config, 2);
        machine_i2c_target_hw_init(self);
        nlr_pop();
    } else {
        // Ensure partially-initialized instances are fully released on init failure.
        i2c_hw_deinit(self->i2c_obj);
        i2c_free(self->i2c_obj);
        nlr_raise(nlr.ret_val);
    }
}

static mp_obj_t mp_machine_i2c_target_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    /**
     * Only the constructor takes the id.
     * Its validation together with the rest of the arguments is
     * delegated to machine_i2c_target_init_impl() which also allocates
     * the object self = NULL.
    */
    int i2c_id = MACHINE_PIN_AF_UNIT_NONE;
    size_t init_n_args = n_args;
    const mp_obj_t *init_args = args;
    if (n_args > 0 && mp_obj_is_int(args[0])) {
        i2c_id = mp_obj_get_int(args[0]);
        if (i2c_id < 0 || i2c_id >= MICROPY_PY_SCB_NUM_ENTRIES) {
            mp_raise_ValueError(MP_ERROR_TEXT("I2C id out of range"));
        }
        init_n_args = n_args - 1;
        init_args = args + 1;
    } else if (n_args > 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("I2C id must be an integer"));
    }

    machine_i2c_target_obj_t *self = machine_i2c_target_obj_get(i2c_id);
    bool is_new = self == NULL;
    if (is_new) {
        self = machine_i2c_target_obj_alloc();
    }
    if (self == NULL) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("failed to allocate I2CTarget object"));
    }
    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        self->i2c_obj = NULL;
        machine_i2c_target_init_impl(self, i2c_id, init_n_args, init_args, &kw_args);
        nlr_pop();
    } else {
        if (is_new) {
            machine_i2c_target_obj_free(self);
        }
        nlr_raise(nlr.ret_val);
    }

    return MP_OBJ_FROM_PTR(self);
}

static void mp_machine_i2c_target_deinit(machine_i2c_target_obj_t *self) {
    i2c_hw_deinit(self->i2c_obj);
    i2c_free(self->i2c_obj);
    machine_i2c_target_obj_free(self);

    DEBUG_printf("I2C Target deinitialized\n");
}

void machine_i2c_target_deinit_all(void) {
    for (uint8_t i = 0; i < MICROPY_PY_MACHINE_I2C_NUM_ENTRIES; i++) {
        if (machine_i2c_target_obj[i] != NULL) {
            mp_machine_i2c_target_deinit(machine_i2c_target_obj[i]);
        }
    }
}

static void mp_machine_i2c_target_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_i2c_target_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "I2CTarget(%u, addr=0x%02X, scl='%q', sda='%q')",
        i2c_get_id(self->i2c_obj), self->addr, self->scl->name, self->sda->name);
}
