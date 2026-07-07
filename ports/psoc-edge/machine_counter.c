/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Infineon Technologies AG
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

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mphal.h"

#include "modmachine.h"
#include "machine_pin.h"
#include "tcpwm.h"

#include "cy_gpio.h"
#include "cy_sysclk.h"
#include "cy_trigmux.h"
#include "cy_tcpwm_counter.h"
#include "cy_tcpwm.h"
#include "cycfg_peripheral_clocks.h"
#include "mtb_hal.h"

#include "genhdr/pins_af.h"

// Number of Counter object slots supported by this port.
#define MACHINE_COUNTER_NUM_INSTANCES  (32)
// Shared TCPWM source clock frequency used by Counter.
#define COUNTER_CLK_HZ                 (1000000UL)
// Value used to disable optional TCPWM trigger inputs.
#define COUNTER_INPUT_DISABLED         (0x7U)

typedef enum {
    COUNTER_EDGE_RISING = 1,
    COUNTER_EDGE_FALLING = 2,
} machine_counter_edge_t;

typedef enum {
    COUNTER_DIR_UP = 1,
    COUNTER_DIR_DOWN = 2,
} machine_counter_dir_t;

typedef struct _machine_counter_obj_t {
    mp_obj_base_t base;
    uint8_t id;
    uint32_t counter_num;
    en_clk_dst_t pclk_dst;
    const machine_pin_obj_t *src_pin;
    en_hsiom_sel_t src_hsiom;
    uint8_t edge;
    uint8_t direction;
    mp_int_t offset;
    bool configured;
} machine_counter_obj_t;

// One Counter object per hardware id.
static machine_counter_obj_t *counter_obj[MACHINE_COUNTER_NUM_INSTANCES] = { NULL };
static bool machine_counter_clock_configured = false;

// ---------------------------------------------------------------------------
// Per-ID hardware mapping from generated AF data.
// ---------------------------------------------------------------------------

// Expands generated map entries into id->hardware-counter lookup.
#define COUNTER_HW_ENTRY(id, counter, irq, pclk, trig) counter,
static const uint32_t counter_hw[MACHINE_COUNTER_NUM_INSTANCES] = {
    MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_HW_ENTRY)
};
#undef COUNTER_HW_ENTRY

// Expands generated map entries into id->output-trigger-line lookup.
#define COUNTER_OUT_TRIG_ENTRY(id, counter, irq, pclk, trig) trig,
static const uint32_t counter_out_trig[MACHINE_COUNTER_NUM_INSTANCES] = {
    MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_OUT_TRIG_ENTRY)
};
#undef COUNTER_OUT_TRIG_ENTRY

// This mapping is used to route a TCPWM0 counter's output trigger to a GPIO pin.
// Each row: X(port_num, pin_num, in_trig_line, pin_hsiom)
#define MACHINE_COUNTER_PIN_TRIGGER_MAP(X) \
    X(11, 0, PERI_0_TRIG_IN_MUX_3_PERI0_HSIOM_TR_OUT0, P11_0_PERI0_TR_IO_INPUT0) \
    X(11, 1, PERI_0_TRIG_IN_MUX_3_PERI0_HSIOM_TR_OUT1, P11_1_PERI0_TR_IO_INPUT1) \
    X(11, 2, PERI_0_TRIG_IN_MUX_3_PERI0_HSIOM_TR_OUT2, P11_2_PERI0_TR_IO_INPUT2) \
    X(11, 3, PERI_0_TRIG_IN_MUX_3_PERI0_HSIOM_TR_OUT3, P11_3_PERI0_TR_IO_INPUT3) \
    X(7, 5, PERI_0_TRIG_IN_MUX_3_PERI0_HSIOM_TR_OUT4, P7_5_PERI0_TR_IO_INPUT4) \
    X(7, 6, PERI_0_TRIG_IN_MUX_3_PERI0_HSIOM_TR_OUT5, P7_6_PERI0_TR_IO_INPUT5) \
    X(7, 7, PERI_0_TRIG_IN_MUX_3_PERI0_HSIOM_TR_OUT6, P7_7_PERI0_TR_IO_INPUT6) \
    X(8, 0, PERI_0_TRIG_IN_MUX_3_PERI0_HSIOM_TR_OUT7, P8_0_PERI0_TR_IO_INPUT7)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Configure the shared peripheral clock once for all Counter objects.
static void machine_counter_configure_clock(void) {
    if (machine_counter_clock_configured) {
        return;
    }

    cy_rslt_t rslt = mtb_hal_clock_set_peri_clock_freq(&CYBSP_GENERAL_PURPOSE_TIMER_clock_ref,
        COUNTER_CLK_HZ, 1000);
    if (rslt != CY_RSLT_SUCCESS) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("Counter clock setup failed (0x%lx)"),
            (unsigned long)rslt);
    }

    machine_counter_clock_configured = true;
}

// Return the max period for this counter width (16-bit or 32-bit).
static uint32_t machine_counter_period_max(uint32_t counter_num) {
    return (counter_num >= 256U) ? 0xFFFFU : UINT32_MAX;
}

typedef struct _machine_counter_pin_trigger_map_t {
    uint8_t port;
    uint8_t pin;
    uint32_t in_trig;
    en_hsiom_sel_t hsiom;
} machine_counter_pin_trigger_map_t;

// Expands pin mapping rows into lookup table entries.
#define PIN_TRIG_ENTRY(port_num, pin_num, in_trig_line, pin_hsiom) \
    { (uint8_t)(port_num), (uint8_t)(pin_num), (in_trig_line), (pin_hsiom) },
static const machine_counter_pin_trigger_map_t machine_counter_pin_trigger_map[] = {
    MACHINE_COUNTER_PIN_TRIGGER_MAP(PIN_TRIG_ENTRY)
};
#undef PIN_TRIG_ENTRY

// Resolve a source pin to its trigger input line and HSIOM function.
static bool machine_counter_pin_to_trigger(const machine_pin_obj_t *pin,
    uint32_t *in_trig, en_hsiom_sel_t *hsiom) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(machine_counter_pin_trigger_map); ++i) {
        const machine_counter_pin_trigger_map_t *entry = &machine_counter_pin_trigger_map[i];
        if (pin->port == entry->port && pin->pin == entry->pin) {
            *in_trig = entry->in_trig;
            *hsiom = entry->hsiom;
            return true;
        }
    }

    return false;
}

// Restore the routed source pin back to GPIO input mode.
static void machine_counter_restore_src_pin(machine_counter_obj_t *self) {
    if (self->src_pin == NULL) {
        return;
    }
    GPIO_PRT_Type *port = Cy_GPIO_PortToAddr(self->src_pin->port);
    Cy_GPIO_SetHSIOM(port, self->src_pin->pin, HSIOM_SEL_GPIO);
    Cy_GPIO_SetDrivemode(port, self->src_pin->pin, CY_GPIO_DM_HIGHZ);
    self->src_pin = NULL;
}

// Disable the underlying TCPWM counter channel.
static void machine_counter_stop_hw(machine_counter_obj_t *self) {
    Cy_TCPWM_Counter_Disable(TCPWM0, self->counter_num);
}

// Parse init args and configure routing, trigger mux, and TCPWM hardware.
static void machine_counter_init_helper(machine_counter_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    enum { ARG_src, ARG_edge, ARG_direction };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_src, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_edge, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = COUNTER_EDGE_RISING} },
        { MP_QSTR_direction, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = COUNTER_DIR_UP} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    uint8_t edge = args[ARG_edge].u_int;
    if (edge != COUNTER_EDGE_RISING && edge != COUNTER_EDGE_FALLING) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid edge"));
    }

    uint8_t direction = args[ARG_direction].u_int;
    if (direction != COUNTER_DIR_UP && direction != COUNTER_DIR_DOWN) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid direction"));
    }

    // Resolve source pin object and fetch its trigger-routing metadata.
    const machine_pin_obj_t *src_pin = machine_pin_get_pin_obj(args[ARG_src].u_obj);
    uint32_t in_trig = 0;
    en_hsiom_sel_t hsiom = (en_hsiom_sel_t)0;
    if (!machine_counter_pin_to_trigger(src_pin, &in_trig, &hsiom)) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("Pin %q does not support Counter input routing"), src_pin->name);
    }

    // Tear down any previous run before programming new pin routing.
    machine_counter_stop_hw(self);
    machine_counter_restore_src_pin(self);

    // Ensure shared timer clock is configured and bound to this counter PCLK.
    machine_counter_configure_clock();
    Cy_SysClk_PeriPclkAssignDivider(self->pclk_dst,
        CY_SYSCLK_DIV_16_BIT, CYBSP_GENERAL_PURPOSE_TIMER_CLK_DIV_NUM);

    // Record the source pin now so rollback can restore it on any failure below.
    self->src_pin = src_pin;
    self->src_hsiom = hsiom;

    // Put pin in input mode and switch HSIOM to trigger-output function.
    mp_hal_pin_input(src_pin);
    GPIO_PRT_Type *src_port = Cy_GPIO_PortToAddr(src_pin->port);
    Cy_GPIO_SetHSIOM(src_port, src_pin->pin, hsiom);
    Cy_GPIO_SetDrivemode(src_port, src_pin->pin, CY_GPIO_DM_HIGHZ);

    // Connect selected pin trigger line to this counter's dedicated trigger input.
    uint32_t out_trig = counter_out_trig[self->id];
    cy_en_trigmux_status_t mux_rslt = Cy_TrigMux_Connect(in_trig, out_trig, false, TRIGGER_TYPE_LEVEL);
    if (mux_rslt != CY_TRIGMUX_SUCCESS) {
        // Roll back pin mux state if trigger connect fails.
        machine_counter_restore_src_pin(self);
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("Counter route connect failed (0x%lx)"),
            (unsigned long)mux_rslt);
    }

    cy_stc_tcpwm_counter_config_t cfg = {0};
    cfg.period = machine_counter_period_max(self->counter_num);
    cfg.clockPrescaler = CY_TCPWM_COUNTER_PRESCALER_DIVBY_1;
    cfg.runMode = CY_TCPWM_COUNTER_CONTINUOUS;
    cfg.countDirection = CY_TCPWM_COUNTER_COUNT_UP;
    cfg.compareOrCapture = CY_TCPWM_COUNTER_MODE_COMPARE;
    cfg.compare0 = 0;
    cfg.compare1 = 0;
    cfg.enableCompareSwap = false;
    cfg.interruptSources = 0;
    cfg.captureInputMode = COUNTER_INPUT_DISABLED & 0x3U;
    cfg.captureInput = CY_TCPWM_INPUT_0;
    cfg.reloadInputMode = COUNTER_INPUT_DISABLED & 0x3U;
    cfg.reloadInput = CY_TCPWM_INPUT_0;
    cfg.startInputMode = COUNTER_INPUT_DISABLED & 0x3U;
    cfg.startInput = CY_TCPWM_INPUT_0;
    cfg.stopInputMode = COUNTER_INPUT_DISABLED & 0x3U;
    cfg.stopInput = CY_TCPWM_INPUT_0;
    cfg.countInputMode = (edge == COUNTER_EDGE_RISING) ? CY_TCPWM_INPUT_RISINGEDGE : CY_TCPWM_INPUT_FALLINGEDGE;
    // With TCPWM_TR_ONE_CNT_NR == 1 this selects one-to-one trigger input for this counter.
    cfg.countInput = CY_TCPWM_INPUT_TRIG_0;

    #if (CY_IP_MXTCPWM_VERSION >= 2U)
    cfg.capture1InputMode = COUNTER_INPUT_DISABLED & 0x3U;
    cfg.capture1Input = CY_TCPWM_INPUT_0;
    cfg.enableCompare1Swap = false;
    cfg.compare2 = CY_TCPWM_GRP_CNT_CC0_DEFAULT;
    cfg.compare3 = CY_TCPWM_GRP_CNT_CC0_BUFF_DEFAULT;
    cfg.trigger0Event = CY_TCPWM_CNT_TRIGGER_ON_DISABLED;
    cfg.trigger1Event = CY_TCPWM_CNT_TRIGGER_ON_DISABLED;
    #endif

    cy_en_tcpwm_status_t rslt = Cy_TCPWM_Counter_Init(TCPWM0, self->counter_num, &cfg);
    if (rslt != CY_TCPWM_SUCCESS) {
        machine_counter_restore_src_pin(self);
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("Counter(%u) init failed (PDL error 0x%lx)"),
            (unsigned)self->id, (unsigned long)rslt);
    }

    Cy_TCPWM_Counter_SetCounter(TCPWM0, self->counter_num, 0);
    Cy_TCPWM_Counter_Enable(TCPWM0, self->counter_num);
    Cy_TCPWM_TriggerReloadOrIndex_Single(TCPWM0, self->counter_num);

    self->src_pin = src_pin;
    self->src_hsiom = hsiom;
    self->edge = edge;
    self->direction = direction;
    self->offset = 0;
    self->configured = true;
}

// ---------------------------------------------------------------------------
// Constructor: Counter(id, ...)
// ---------------------------------------------------------------------------

// Create a Counter object, reserve hardware, and run optional init.
static mp_obj_t machine_counter_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {

    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    mp_int_t id = mp_obj_get_int(args[0]);
    if (id < 0 || id >= MACHINE_COUNTER_NUM_INSTANCES) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("Counter id must be in range 0-%d"),
            MACHINE_COUNTER_NUM_INSTANCES - 1);
    }

    if (counter_obj[id] != NULL) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("Counter(%u) already created."), (unsigned)id);
    }

    machine_counter_obj_t *self = mp_obj_malloc(machine_counter_obj_t, &machine_counter_type);
    counter_obj[id] = self;

    self->id = (uint8_t)id;
    self->counter_num = counter_hw[id];
    self->pclk_dst = machine_tcpwm_counter_pclk(self->counter_num);
    self->src_pin = NULL;
    self->src_hsiom = (en_hsiom_sel_t)0;
    self->edge = COUNTER_EDGE_RISING;
    self->direction = COUNTER_DIR_UP;
    self->offset = 0;
    self->configured = false;

    nlr_buf_t nl;
    if (nlr_push(&nl) == 0) {
        machine_tcpwm_counter_alloc(self->counter_num, MP_OBJ_FROM_PTR(self));
        if (n_args > 1 || n_kw > 0) {
            mp_map_t kw_map;
            mp_map_init_fixed_table(&kw_map, n_kw, args + n_args);
            machine_counter_init_helper(self, n_args - 1, args + 1, &kw_map);
        }
        nlr_pop();
    } else {
        machine_tcpwm_counter_free(self->counter_num, MP_OBJ_FROM_PTR(self));
        counter_obj[id] = NULL;
        nlr_jump(nl.ret_val);
    }

    return MP_OBJ_FROM_PTR(self);
}

// ---------------------------------------------------------------------------
// counter.init(src, *, edge=, direction=)
// ---------------------------------------------------------------------------

// Python binding for Counter.init(...).
static mp_obj_t machine_counter_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    machine_counter_init_helper(MP_OBJ_TO_PTR(args[0]), n_args - 1, args + 1, kw_args);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_counter_init_obj, 1, machine_counter_init);

// ---------------------------------------------------------------------------
// counter.deinit()
// ---------------------------------------------------------------------------

// Python binding for Counter.deinit().
static mp_obj_t machine_counter_deinit(mp_obj_t self_in) {
    machine_counter_obj_t *self = MP_OBJ_TO_PTR(self_in);

    machine_counter_stop_hw(self);
    machine_counter_restore_src_pin(self);

    self->configured = false;
    self->offset = 0;

    machine_tcpwm_counter_free(self->counter_num, MP_OBJ_FROM_PTR(self));
    counter_obj[self->id] = NULL;

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_counter_deinit_obj, machine_counter_deinit);

// Module-level lifecycle called from machine_deinit().
void machine_counter_deinit_all(void) {
    for (uint8_t i = 0; i < MACHINE_COUNTER_NUM_INSTANCES; i++) {
        machine_counter_obj_t *self = counter_obj[i];
        if (self != NULL) {
            machine_counter_deinit(MP_OBJ_FROM_PTR(self));
        }
    }
}

// ---------------------------------------------------------------------------
// counter.value([value])
// ---------------------------------------------------------------------------

// Return current count, or reset logical origin when a value is provided.
static mp_obj_t machine_counter_value(size_t n_args, const mp_obj_t *args) {
    machine_counter_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (!self->configured) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialised"));
    }

    if (n_args == 2) {
        // Freeze counting during read/reset so no new edges arrive in this window.
        Cy_TCPWM_Counter_Disable(TCPWM0, self->counter_num);
    }
    uint32_t raw = Cy_TCPWM_Counter_GetCounter(TCPWM0, self->counter_num);
    long long signed_count = (self->direction == COUNTER_DIR_DOWN) ? -(long long)raw : (long long)raw;
    long long result = (long long)self->offset + signed_count;

    if (n_args == 2) {
        self->offset = mp_obj_get_int(args[1]);
        Cy_TCPWM_Counter_SetCounter(TCPWM0, self->counter_num, 0);
        Cy_TCPWM_Counter_Enable(TCPWM0, self->counter_num);
        Cy_TCPWM_TriggerReloadOrIndex_Single(TCPWM0, self->counter_num);
    }

    return mp_obj_new_int_from_ll(result);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_counter_value_obj, 1, 2, machine_counter_value);

// Print user-facing representation as Counter(<id>).
static void machine_counter_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_counter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *edge_str = (self->edge == COUNTER_EDGE_FALLING) ? "FALLING" : "RISING";
    const char *dir_str = (self->direction == COUNTER_DIR_DOWN) ? "DOWN" : "UP";

    if (self->src_pin == NULL) {
        mp_printf(print, "Counter(id=%u, src=None, edge=Counter.%s, direction=Counter.%s)",
            (unsigned)self->id, edge_str, dir_str);
    } else {
        mp_printf(print, "Counter(id=%u, src=%q, edge=Counter.%s, direction=Counter.%s)",
            (unsigned)self->id, self->src_pin->name, edge_str, dir_str);
    }
}

static const mp_rom_map_elem_t machine_counter_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&machine_counter_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&machine_counter_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&machine_counter_value_obj) },

    { MP_ROM_QSTR(MP_QSTR_RISING), MP_ROM_INT(COUNTER_EDGE_RISING) },
    { MP_ROM_QSTR(MP_QSTR_FALLING), MP_ROM_INT(COUNTER_EDGE_FALLING) },
    { MP_ROM_QSTR(MP_QSTR_UP), MP_ROM_INT(COUNTER_DIR_UP) },
    { MP_ROM_QSTR(MP_QSTR_DOWN), MP_ROM_INT(COUNTER_DIR_DOWN) },
};
static MP_DEFINE_CONST_DICT(machine_counter_locals_dict, machine_counter_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_counter_type,
    MP_QSTR_Counter,
    MP_TYPE_FLAG_NONE,
    make_new, machine_counter_make_new,
    print, machine_counter_print,
    locals_dict, &machine_counter_locals_dict
    );
