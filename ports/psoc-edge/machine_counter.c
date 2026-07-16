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
#include "shared/runtime/mpirq.h"

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
#include "sys_int.h"

// Reuse machine.Pin IRQ API to implement Counter index input callbacks.
extern mp_obj_t machine_pin_irq(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args);

// Forward declaration so init-failure path can reuse deinit logic.
static mp_obj_t machine_counter_deinit(mp_obj_t self_in);

// Number of Counter object slots supported by this port.
#define MACHINE_COUNTER_NUM_INSTANCES  MICROPY_PY_MACHINE_TCPWM0_NUM_COUNTERS
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

#define MACHINE_COUNTER_IRQ_RESET      (1U << 0)
#define MACHINE_COUNTER_IRQ_INDEX      (1U << 1)
#define MACHINE_COUNTER_IRQ_MATCH      (1U << 2)
#define MACHINE_COUNTER_IRQ_ROLL_UNDER (1U << 3)
#define MACHINE_COUNTER_IRQ_ROLL_OVER  (1U << 4)
#define MACHINE_COUNTER_ALLOWED_FLAGS  (MACHINE_COUNTER_IRQ_RESET \
    | MACHINE_COUNTER_IRQ_INDEX \
    | MACHINE_COUNTER_IRQ_MATCH \
    | MACHINE_COUNTER_IRQ_ROLL_UNDER \
    | MACHINE_COUNTER_IRQ_ROLL_OVER)

typedef struct _machine_counter_obj_t {
    mp_obj_base_t base;
    uint8_t id;
    uint32_t counter_num;
    en_clk_dst_t pclk_dst;
    mp_hal_pin_obj_t src_pin;
    mp_hal_pin_obj_t index_pin;
    mp_hal_pin_obj_t reset_pin;
    uint32_t match_value;
    bool match_enabled;
    uint8_t edge;
    uint8_t direction;
    uint32_t range_min;
    uint32_t range_max;
    bool range_min_negative;
    bool range_max_negative;
    uint16_t cycles_u16;
    mp_int_t offset;
    mp_obj_t index_irq_obj;
    mp_obj_t reset_irq_obj;
    mp_irq_obj_t *mp_irq_obj;
    mp_uint_t mp_irq_flags;
    mp_uint_t mp_irq_trigger;
    sys_int_cfg_t irq_cfg;
    bool irq_active;
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

// Expands generated map entries into id->counter-IRQ lookup.
#define COUNTER_IRQ_ENTRY(id, counter, irq, pclk, trig) irq,
static const IRQn_Type counter_irq[MACHINE_COUNTER_NUM_INSTANCES] = {
    MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_IRQ_ENTRY)
};
#undef COUNTER_IRQ_ENTRY

// ---------------------------------------------------------------------------
// Core Helpers
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

/**
 * TODO:
 * The TrigMux functionality might be required for other modules,
 * and these definitions might be move in the future to a separate
 * c file to be shared by other modules.
 * Only PERI0 is used here, but PERI1 is also available and could
 * be used.
 */
#define MAP_COUNTER_PIN_INPUT_TRIGGER(id) \
    [id] = PERI_0_TRIG_IN_MUX_3_PERI0_HSIOM_TR_OUT##id,

static const en_peri0_trig_input_debugreducation1_t peri0_tr_io_input[MICROPY_PY_MACHINE_PERI0_TR_IO_INPUT_NUM_ENTRIES] = {
    MICROPY_PY_MACHINE_FOR_ALL_PERI0_TR_IO_INPUTS(MAP_COUNTER_PIN_INPUT_TRIGGER)
};

// Restore the routed source pin back to GPIO input mode.
static void machine_counter_restore_src_pin(machine_counter_obj_t *self) {
    if (self->src_pin == NULL) {
        return;
    }
    // Use mphalport abstraction to restore pin to input mode (GPIO, high-impedance)
    mp_hal_pin_input(self->src_pin);
    self->src_pin = NULL;
}

// Disable the underlying TCPWM counter channel.
static void machine_counter_stop_hw(machine_counter_obj_t *self) {
    Cy_TCPWM_Counter_Disable(TCPWM0, self->counter_num);
}

// Disable index/reset pin IRQs (if configured) and release pin ownership.
static void machine_counter_disable_aux_irqs(machine_counter_obj_t *self) {
    // Disable index pin IRQs (if configured) and release pin ownership.
    if (self->index_pin != NULL && self->index_irq_obj != MP_OBJ_NULL) {
        mp_obj_t pos_args[] = {
            MP_OBJ_FROM_PTR(self->index_pin),
            mp_const_none,
        };
        mp_map_t kw_args;
        mp_map_init_fixed_table(&kw_args, 0, NULL);
        machine_pin_irq(2, pos_args, &kw_args);
    }

    // Disable reset pin IRQs (if configured) and release pin ownership.
    if (self->reset_pin != NULL && self->reset_irq_obj != MP_OBJ_NULL
        && self->reset_pin != self->index_pin) {
        mp_obj_t pos_args[] = {
            MP_OBJ_FROM_PTR(self->reset_pin),
            mp_const_none,
        };
        mp_map_t kw_args;
        mp_map_init_fixed_table(&kw_args, 0, NULL);
        machine_pin_irq(2, pos_args, &kw_args);
    }

    self->index_pin = NULL;
    self->reset_pin = NULL;
    self->index_irq_obj = MP_OBJ_NULL;
    self->reset_irq_obj = MP_OBJ_NULL;
}

// Disable and unregister the counter IRQ if it is currently active.
static void machine_counter_stop_irq(machine_counter_obj_t *self) {
    if (self->irq_active) {
        sys_int_deinit(&self->irq_cfg);
        self->irq_active = false;
    }
}

// Return the cycles counter as signed 16-bit value.
static inline int16_t machine_counter_cycles_get(const machine_counter_obj_t *self) {
    return (int16_t)self->cycles_u16;
}

static inline int64_t machine_counter_range_min_value(const machine_counter_obj_t *self) {
    // Reinterpret the stored raw bits as signed when the configured min was negative,
    // then widen to int64_t so later arithmetic uses the intended logical value.
    return self->range_min_negative ? (int64_t)(int32_t)self->range_min : (int64_t)self->range_min;
}

static inline int64_t machine_counter_range_max_value(const machine_counter_obj_t *self) {
    // Reinterpret the stored raw bits as signed when the configured max was negative,
    // then widen to int64_t so later arithmetic uses the intended logical value.
    return self->range_max_negative ? (int64_t)(int32_t)self->range_max : (int64_t)self->range_max;
}

static inline int64_t machine_counter_range_span_value(const machine_counter_obj_t *self) {
    return machine_counter_range_max_value(self) - machine_counter_range_min_value(self) + 1;
}

static inline uint32_t machine_counter_interrupt_mask(const machine_counter_obj_t *self) {
    return CY_TCPWM_INT_ON_TC
           | ((self->match_enabled && ((self->mp_irq_trigger & MACHINE_COUNTER_IRQ_MATCH) != 0))
            ? CY_TCPWM_INT_ON_CC0
            : 0U);
}

static inline void machine_counter_apply_interrupt_mask(const machine_counter_obj_t *self) {
    Cy_TCPWM_SetInterruptMask(TCPWM0, self->counter_num, machine_counter_interrupt_mask(self));
}

static inline void clear_counter_irqs(const machine_counter_obj_t *self, uint32_t mask) {
    Cy_TCPWM_ClearInterrupt(TCPWM0, self->counter_num, mask);
}

// Increment/decrement cycles counter with 16-bit wrap semantics.
static inline void machine_counter_cycles_step(machine_counter_obj_t *self) {
    if (self->direction == COUNTER_DIR_DOWN) {
        self->cycles_u16--;
    } else {
        self->cycles_u16++;
    }
}

// ---------------------------------------------------------------------------
// IRQ Internals
// ---------------------------------------------------------------------------

static mp_uint_t machine_counter_irq_trigger(mp_obj_t self_in, mp_uint_t new_trigger) {
    machine_counter_obj_t *self = MP_OBJ_TO_PTR(self_in);

    mp_uint_t irq_state = MICROPY_BEGIN_ATOMIC_SECTION();
    self->mp_irq_trigger = new_trigger;
    machine_counter_apply_interrupt_mask(self);
    MICROPY_END_ATOMIC_SECTION(irq_state);

    return 0;
}

static mp_uint_t machine_counter_irq_info(mp_obj_t self_in, mp_uint_t info_type) {
    machine_counter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (info_type == MP_IRQ_INFO_FLAGS) {
        return self->mp_irq_flags;
    } else if (info_type == MP_IRQ_INFO_TRIGGERS) {
        return self->mp_irq_trigger;
    }
    return 0;
}

static const mp_irq_methods_t machine_counter_irq_methods = {
    .trigger = machine_counter_irq_trigger,
    .info = machine_counter_irq_info,
};

static inline void machine_counter_irq_raise(machine_counter_obj_t *self, mp_uint_t flags) {
    if (self->mp_irq_obj == NULL) {
        return;
    }

    mp_uint_t irq_state = MICROPY_BEGIN_ATOMIC_SECTION();
    self->mp_irq_flags = flags;
    bool call_handler = (self->mp_irq_obj->handler != mp_const_none)
        && ((self->mp_irq_trigger & flags) != 0);
    // MATCH is one-shot: clear its trigger bit after raising so user must re-arm.
    self->mp_irq_trigger &= ~(flags & MACHINE_COUNTER_IRQ_MATCH);
    if ((flags & MACHINE_COUNTER_IRQ_MATCH) != 0) {
        machine_counter_apply_interrupt_mask(self);
    }
    MICROPY_END_ATOMIC_SECTION(irq_state);

    if (call_handler) {
        mp_irq_handler(self->mp_irq_obj);
    }
}

// Counter terminal-count ISR: track wrap events in software cycles.
static void machine_counter_isr(machine_counter_obj_t *self) {
    uint32_t status = Cy_TCPWM_GetInterruptStatus(TCPWM0, self->counter_num);

    if (status & CY_TCPWM_INT_ON_CC0) {
        clear_counter_irqs(self, CY_TCPWM_INT_ON_CC0);
        machine_counter_irq_raise(self, MACHINE_COUNTER_IRQ_MATCH);
    }

    if (status & CY_TCPWM_INT_ON_TC) {
        clear_counter_irqs(self, CY_TCPWM_INT_ON_TC);
        machine_counter_cycles_step(self);
        machine_counter_irq_raise(self,
            (self->direction == COUNTER_DIR_DOWN)
                ? MACHINE_COUNTER_IRQ_ROLL_UNDER
                : MACHINE_COUNTER_IRQ_ROLL_OVER);
    }
}

// Shared handler for index/reset pin rising edges.
// If update_cycles is true (index), adjust cycles;
// if false (reset), keep cycles unchanged.
static inline void machine_counter_on_aux_event(machine_counter_obj_t *self, bool update_cycles,
    mp_uint_t irq_flag) {
    if (!self->configured) {
        return;
    }

    mp_uint_t irq_state = MICROPY_BEGIN_ATOMIC_SECTION();
    if (update_cycles) {
        // Update cycles counter on index pin rising edge.
        machine_counter_cycles_step(self);
    }
    Cy_TCPWM_Counter_SetCounter(TCPWM0, self->counter_num, 0);
    clear_counter_irqs(self, CY_TCPWM_INT_ON_TC | CY_TCPWM_INT_ON_CC0);
    MICROPY_END_ATOMIC_SECTION(irq_state);
    machine_counter_irq_raise(self, irq_flag);
}

// -------------------------------------------------------------------------- //
// Generated IRQ/Callback Dispatch Tables
// -------------------------------------------------------------------------- //

// Generate one IRQ handler per Counter id for direct dispatch.
#define COUNTER_IRQ_HANDLER_DECL(id, counter, irq, pclk, trig) \
    static void machine_counter_irq_handler_##id(void) { \
        machine_counter_obj_t *self = counter_obj[id]; \
        if (self != NULL) { \
            machine_counter_isr(self); \
        } \
    }
MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_IRQ_HANDLER_DECL)
#undef COUNTER_IRQ_HANDLER_DECL

// Generate one IRQ handler entry per Counter id for direct dispatch.
#define COUNTER_IRQ_HANDLER_ENTRY(id, counter, irq, pclk, trig) machine_counter_irq_handler_##id,
static const cy_israddress machine_counter_irq_handlers[MACHINE_COUNTER_NUM_INSTANCES] = {
    MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_IRQ_HANDLER_ENTRY)
};
#undef COUNTER_IRQ_HANDLER_ENTRY
// -------------------------------------------------------------------------- //

// -------------------------------------------------------------------------- //
// Generate one index callback per Counter id (bound through machine.Pin.irq).
#define COUNTER_INDEX_HANDLER_DECL(id, counter, irq, pclk, trig) \
    static mp_obj_t machine_counter_index_handler_##id(mp_obj_t pin_in) { \
        (void)pin_in; \
        machine_counter_obj_t *self = counter_obj[id]; \
        if (self != NULL && self->index_pin != NULL) { \
            machine_counter_on_aux_event(self, true, MACHINE_COUNTER_IRQ_INDEX); \
        } \
        return mp_const_none; \
    }
MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_INDEX_HANDLER_DECL)
#undef COUNTER_INDEX_HANDLER_DECL

// Generate one index callback object per Counter id (bound through machine.Pin.irq).
#define COUNTER_INDEX_HANDLER_OBJ_DECL(id, counter, irq, pclk, trig) \
    static MP_DEFINE_CONST_FUN_OBJ_1(machine_counter_index_handler_obj_##id, machine_counter_index_handler_##id);
MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_INDEX_HANDLER_OBJ_DECL)
#undef COUNTER_INDEX_HANDLER_OBJ_DECL

// Generate one index callback object entry per Counter id (bound through machine.Pin.irq).
#define COUNTER_INDEX_HANDLER_OBJ_ENTRY(id, counter, irq, pclk, trig) MP_OBJ_FROM_PTR(&machine_counter_index_handler_obj_##id),
static const mp_obj_t machine_counter_index_handler_obj[MACHINE_COUNTER_NUM_INSTANCES] = {
    MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_INDEX_HANDLER_OBJ_ENTRY)
};
#undef COUNTER_INDEX_HANDLER_OBJ_ENTRY
// -------------------------------------------------------------------------- //

// -------------------------------------------------------------------------- //
// Generate one reset callback per Counter id (bound through machine.Pin.irq).
#define COUNTER_RESET_HANDLER_DECL(id, counter, irq, pclk, trig) \
    static mp_obj_t machine_counter_reset_handler_##id(mp_obj_t pin_in) { \
        (void)pin_in; \
        machine_counter_obj_t *self = counter_obj[id]; \
        if (self != NULL && self->reset_pin != NULL) { \
            machine_counter_on_aux_event(self, false, MACHINE_COUNTER_IRQ_RESET); \
        } \
        return mp_const_none; \
    }
MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_RESET_HANDLER_DECL)
#undef COUNTER_RESET_HANDLER_DECL

// Generate one reset callback object per Counter id (bound through machine.Pin.irq).
#define COUNTER_RESET_HANDLER_OBJ_DECL(id, counter, irq, pclk, trig) \
    static MP_DEFINE_CONST_FUN_OBJ_1(machine_counter_reset_handler_obj_##id, machine_counter_reset_handler_##id);
MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_RESET_HANDLER_OBJ_DECL)
#undef COUNTER_RESET_HANDLER_OBJ_DECL

// Generate one reset callback object entry per Counter id (bound through machine.Pin.irq).
#define COUNTER_RESET_HANDLER_OBJ_ENTRY(id, counter, irq, pclk, trig) MP_OBJ_FROM_PTR(&machine_counter_reset_handler_obj_##id),
static const mp_obj_t machine_counter_reset_handler_obj[MACHINE_COUNTER_NUM_INSTANCES] = {
    MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_RESET_HANDLER_OBJ_ENTRY)
};
#undef COUNTER_RESET_HANDLER_OBJ_ENTRY
// -------------------------------------------------------------------------- //

// -------------------------------------------------------------------------- //
// If index and reset share a pin, run both semantics from a single callback.
#define COUNTER_INDEX_RESET_HANDLER_DECL(id, counter, irq, pclk, trig) \
    static mp_obj_t machine_counter_index_reset_handler_##id(mp_obj_t pin_in) { \
        (void)pin_in; \
        machine_counter_obj_t *self = counter_obj[id]; \
        if (self != NULL) { \
            if (self->index_pin != NULL) { \
                machine_counter_on_aux_event(self, true, MACHINE_COUNTER_IRQ_INDEX); \
            } \
            if (self->reset_pin != NULL) { \
                machine_counter_on_aux_event(self, false, MACHINE_COUNTER_IRQ_RESET); \
            } \
        } \
        return mp_const_none; \
    }
MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_INDEX_RESET_HANDLER_DECL)
#undef COUNTER_INDEX_RESET_HANDLER_DECL

// Generate one index/reset callback object per Counter id (bound through machine.Pin.irq).
#define COUNTER_INDEX_RESET_HANDLER_OBJ_DECL(id, counter, irq, pclk, trig) \
    static MP_DEFINE_CONST_FUN_OBJ_1(machine_counter_index_reset_handler_obj_##id, machine_counter_index_reset_handler_##id);
MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_INDEX_RESET_HANDLER_OBJ_DECL)
#undef COUNTER_INDEX_RESET_HANDLER_OBJ_DECL

// Generate one index/reset callback object entry per Counter id (bound through machine.Pin.irq).
#define COUNTER_INDEX_RESET_HANDLER_OBJ_ENTRY(id, counter, irq, pclk, trig) MP_OBJ_FROM_PTR(&machine_counter_index_reset_handler_obj_##id),
static const mp_obj_t machine_counter_index_reset_handler_obj[MACHINE_COUNTER_NUM_INSTANCES] = {
    MICROPY_PY_MACHINE_TCPWM_MAP(COUNTER_INDEX_RESET_HANDLER_OBJ_ENTRY)
};
#undef COUNTER_INDEX_RESET_HANDLER_OBJ_ENTRY

// -------------------------------------------------------------------------- //

// ---------------------------------------------------------------------------
// Init Internals
// ---------------------------------------------------------------------------

static mp_obj_t machine_counter_enable_aux_irq(mp_hal_pin_obj_t pin, mp_obj_t handler_obj) {
    mp_hal_pin_input(pin);

    mp_obj_t pos_args[] = {
        MP_OBJ_FROM_PTR(pin),
        handler_obj,
    };
    mp_obj_t kw_table[] = {
        MP_OBJ_NEW_QSTR(MP_QSTR_trigger), MP_OBJ_NEW_SMALL_INT(CY_GPIO_INTR_RISING),
        MP_OBJ_NEW_QSTR(MP_QSTR_hard), mp_const_true,
    };
    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, MP_ARRAY_SIZE(kw_table) / 2, kw_table);
    return machine_pin_irq(2, pos_args, &kw_args);
}

// Parse init args and configure routing, trigger mux, and TCPWM hardware.
static void machine_counter_init_helper_impl(machine_counter_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    enum {
        ARG_src,
        ARG_edge,
        ARG_direction,
        ARG_filter_ns,  // currently not supported
        ARG_max,
        ARG_min,
        ARG_index,
        ARG_reset,
        ARG_match,
        ARG_match_pin,  // currently not supported
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_src, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_edge, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = COUNTER_EDGE_RISING} },
        { MP_QSTR_direction, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = COUNTER_DIR_UP} },
        { MP_QSTR_filter_ns, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_max, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_min, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_index, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_reset, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_match, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_match_pin, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
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

    mp_int_t filter_ns = args[ARG_filter_ns].u_int;
    if (filter_ns < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("filter_ns must be >= 0"));
    }
    if (filter_ns > 0) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("filter_ns not supported"));
    }

    mp_int_t min = (mp_int_t)args[ARG_min].u_int;
    int64_t min_value = (int64_t)min;

    mp_hal_pin_obj_t index_pin = NULL;
    if (args[ARG_index].u_obj != mp_const_none) {
        index_pin = mp_hal_get_pin_obj(args[ARG_index].u_obj);
    }
    mp_hal_pin_obj_t reset_pin = NULL;
    if (args[ARG_reset].u_obj != mp_const_none) {
        reset_pin = mp_hal_get_pin_obj(args[ARG_reset].u_obj);
    }

    if (args[ARG_match_pin].u_obj != mp_const_none) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("match_pin not supported"));
    }

    uint32_t max_hw = machine_counter_period_max(self->counter_num);
    uint32_t range_max = max_hw;
    int64_t max_value = (int64_t)max_hw;
    if (args[ARG_max].u_obj != mp_const_none) {
        mp_int_t max = mp_obj_get_int(args[ARG_max].u_obj);
        if (max == 0 && min == 0) {
            range_max = max_hw;
            max_value = (int64_t)max_hw;
        } else if (max > 0 && (uint64_t)max > (uint64_t)max_hw) {
            mp_raise_ValueError(MP_ERROR_TEXT("max out of range"));
        } else {
            range_max = (uint32_t)max;
            max_value = (int64_t)max;
        }
    }

    if (min_value >= max_value) {
        mp_raise_ValueError(MP_ERROR_TEXT("min must be < max"));
    }

    uint32_t range_min = (uint32_t)min;
    // Compute the logical span in signed space first so negative min/max values remain valid,
    // then reject anything wider than the hardware period before narrowing to uint32_t.
    int64_t period_value = max_value - min_value;
    if (period_value > (int64_t)max_hw) {
        mp_raise_ValueError(MP_ERROR_TEXT("range span out of range"));
    }
    uint32_t period = (uint32_t)period_value;
    bool match_enabled = false;
    uint32_t match_value = 0;

    if (args[ARG_match].u_obj != mp_const_none) {
        mp_int_t match = mp_obj_get_int(args[ARG_match].u_obj);
        int64_t match_value_num = (int64_t)match;
        match_enabled = true;
        match_value = (uint32_t)(match_value_num - min_value);
    }

    // Resolve source pin object and validate it exposes Counter input AF.
    mp_hal_pin_obj_t src_pin = mp_hal_get_pin_obj(args[ARG_src].u_obj);

    mp_hal_pin_af_config_t src_pin_af_config = MP_HAL_PIN_AF_CONF_INIT(src_pin, CY_GPIO_DM_HIGHZ, 0, MACHINE_PIN_AF_SIGNAL_PERI_TR_IO_INPUT);

    machine_pin_af_unit_t fn_unit = MACHINE_PIN_AF_UNIT_NONE;
    mp_hal_periph_pins_af_resolve_fn_unit(&src_pin_af_config, 1, MACHINE_PIN_AF_FN_PERI_TR_IO, &fn_unit);

    en_peri0_trig_input_debugreducation1_t in_trig = peri0_tr_io_input[fn_unit];

    // Tear down any previous run before programming new pin routing.
    machine_counter_stop_hw(self);
    machine_counter_stop_irq(self);
    machine_counter_disable_aux_irqs(self);
    machine_counter_restore_src_pin(self);

    // Ensure shared timer clock is configured and bound to this counter PCLK.
    machine_counter_configure_clock();
    Cy_SysClk_PeriPclkAssignDivider(self->pclk_dst,
        CY_SYSCLK_DIV_16_BIT, CYBSP_GENERAL_PURPOSE_TIMER_CLK_DIV_NUM);

    mp_hal_periph_pins_af_init(&src_pin_af_config, 1);

    // Record the source pin now so rollback can restore it on any failure below.
    self->src_pin = src_pin;
    self->index_pin = index_pin;
    self->reset_pin = reset_pin;

    // Connect selected pin trigger line to this counter's dedicated trigger input.
    uint32_t out_trig = counter_out_trig[self->id];
    cy_en_trigmux_status_t mux_rslt = Cy_TrigMux_Connect(in_trig, out_trig, false, TRIGGER_TYPE_LEVEL);
    if (mux_rslt != CY_TRIGMUX_SUCCESS) {
        // Roll back pin mux state if trigger connect fails.
        machine_counter_restore_src_pin(self);
        machine_counter_disable_aux_irqs(self);
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("Counter route connect failed (0x%lx)"),
            (unsigned long)mux_rslt);
    }

    bool index_reset_same_pin = (self->index_pin != NULL
        && self->reset_pin != NULL
        && self->index_pin == self->reset_pin);

    if (self->index_pin != NULL) {
        self->index_irq_obj = machine_counter_enable_aux_irq(
            self->index_pin,
            index_reset_same_pin
                ? machine_counter_index_reset_handler_obj[self->id]
                : machine_counter_index_handler_obj[self->id]);
        if (index_reset_same_pin) {
            self->reset_irq_obj = self->index_irq_obj;
        }
    }

    if (self->reset_pin != NULL && !index_reset_same_pin) {
        self->reset_irq_obj = machine_counter_enable_aux_irq(
            self->reset_pin,
            machine_counter_reset_handler_obj[self->id]);
    }

    cy_stc_tcpwm_counter_config_t cfg = {0};
    cfg.period = period;
    cfg.clockPrescaler = CY_TCPWM_COUNTER_PRESCALER_DIVBY_1;
    cfg.runMode = CY_TCPWM_COUNTER_CONTINUOUS;
    cfg.countDirection = CY_TCPWM_COUNTER_COUNT_UP;
    cfg.compareOrCapture = CY_TCPWM_COUNTER_MODE_COMPARE;
    cfg.compare0 = match_value;
    cfg.compare1 = 0;
    cfg.enableCompareSwap = false;
    cfg.interruptSources = CY_TCPWM_INT_ON_TC | (match_enabled ? CY_TCPWM_INT_ON_CC0 : 0U);
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
    clear_counter_irqs(self, CY_TCPWM_INT_ON_TC | CY_TCPWM_INT_ON_CC0);

    self->irq_cfg.priority = SYS_INT_IRQ_LOWEST_PRIORITY;
    self->irq_cfg.handler = machine_counter_irq_handlers[self->id];
    sys_int_init(&self->irq_cfg);
    self->irq_active = true;

    Cy_TCPWM_Counter_Enable(TCPWM0, self->counter_num);
    Cy_TCPWM_TriggerReloadOrIndex_Single(TCPWM0, self->counter_num);

    self->edge = edge;
    self->direction = direction;
    self->range_min = range_min;
    self->range_max = range_max;
    self->range_min_negative = (min_value < 0);
    self->range_max_negative = (max_value < 0);
    self->match_enabled = match_enabled;
    self->match_value = match_value;
    self->cycles_u16 = 0;
    self->offset = min;
    machine_counter_apply_interrupt_mask(self);
    self->configured = true;
}

// Wrapper that enforces identical rollback semantics for constructor and init().
static void machine_counter_init_helper(machine_counter_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    nlr_buf_t nl;
    if (nlr_push(&nl) == 0) {
        machine_counter_init_helper_impl(self, n_args, pos_args, kw_args);
        nlr_pop();
    } else {
        machine_counter_deinit(MP_OBJ_FROM_PTR(self));
        nlr_jump(nl.ret_val);
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Public Lifecycle API
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

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
    self->index_pin = NULL;
    self->reset_pin = NULL;
    self->match_value = 0;
    self->match_enabled = false;
    self->edge = COUNTER_EDGE_RISING;
    self->direction = COUNTER_DIR_UP;
    self->range_min = 0;
    self->range_max = machine_counter_period_max(self->counter_num);
    self->range_min_negative = false;
    self->range_max_negative = false;
    self->cycles_u16 = 0;
    self->offset = 0;
    self->index_irq_obj = MP_OBJ_NULL;
    self->reset_irq_obj = MP_OBJ_NULL;
    self->mp_irq_obj = NULL;
    self->mp_irq_flags = 0;
    self->mp_irq_trigger = 0;
    self->irq_cfg.irq_num = counter_irq[id];
    self->irq_active = false;
    self->configured = false;

    nlr_buf_t nl;
    if (nlr_push(&nl) == 0) {
        machine_tcpwm_counter_alloc(self->counter_num, MP_OBJ_FROM_PTR(self));
        nlr_pop();
    } else {
        counter_obj[id] = NULL;
        nlr_jump(nl.ret_val);
    }

    if (n_args > 1 || n_kw > 0) {
        mp_map_t kw_map;
        mp_map_init_fixed_table(&kw_map, n_kw, args + n_args);
        machine_counter_init_helper(self, n_args - 1, args + 1, &kw_map);
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
    machine_counter_stop_irq(self);
    machine_counter_disable_aux_irqs(self);
    machine_counter_restore_src_pin(self);

    self->configured = false;
    self->cycles_u16 = 0;
    self->offset = 0;
    self->match_enabled = false;
    self->match_value = 0;
    self->mp_irq_flags = 0;
    self->mp_irq_trigger = 0;
    if (self->mp_irq_obj != NULL) {
        self->mp_irq_obj->handler = mp_const_none;
    }

    machine_tcpwm_counter_free(self->counter_num, MP_OBJ_FROM_PTR(self));
    if (counter_obj[self->id] == self) {
        counter_obj[self->id] = NULL;
    }

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
// ---------------------------------------------------------------------------
// Public Data/IRQ API
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// counter.value([value])
// ---------------------------------------------------------------------------
// Return current count, or reset logical origin when a value is provided.
static mp_obj_t machine_counter_value(size_t n_args, const mp_obj_t *args) {
    machine_counter_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (!self->configured) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialised"));
    }

    // Get the span of the counter range.
    uint64_t span = (uint64_t)machine_counter_range_span_value(self);

    // If a value is provided, freeze counting during read/reset
    if (n_args == 2) {
        // Freeze counting during read/reset so no new edges arrive in this window.
        Cy_TCPWM_Counter_Disable(TCPWM0, self->counter_num);
    }

    mp_uint_t irq_state = MICROPY_BEGIN_ATOMIC_SECTION();
    uint32_t raw = Cy_TCPWM_Counter_GetCounter(TCPWM0, self->counter_num);
    int16_t cycles = machine_counter_cycles_get(self);
    MICROPY_END_ATOMIC_SECTION(irq_state);

    long long edge_total;
    if (self->direction == COUNTER_DIR_DOWN) {
        edge_total = (long long)(-cycles) * (long long)span + (long long)raw;
        edge_total = -edge_total;
    } else {
        edge_total = (long long)cycles * (long long)span + (long long)raw;
    }
    long long result = (long long)self->offset + edge_total;

    if (n_args == 2) {
        self->offset = mp_obj_get_int(args[1]);
        self->cycles_u16 = 0;
        Cy_TCPWM_Counter_SetCounter(TCPWM0, self->counter_num, 0);
        clear_counter_irqs(self, CY_TCPWM_INT_ON_TC | CY_TCPWM_INT_ON_CC0);
        Cy_TCPWM_Counter_Enable(TCPWM0, self->counter_num);
        Cy_TCPWM_TriggerReloadOrIndex_Single(TCPWM0, self->counter_num);
    }

    return mp_obj_new_int_from_ll(result);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_counter_value_obj, 1, 2, machine_counter_value);

// ---------------------------------------------------------------------------
// counter.cycles([value])
// ---------------------------------------------------------------------------
// Get or set the signed 16-bit software cycles counter.
static mp_obj_t machine_counter_cycles(size_t n_args, const mp_obj_t *args) {
    machine_counter_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (!self->configured) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialised"));
    }

    // set cycles value if provided and also returning previous value.
    if (n_args == 2) {
        mp_int_t value = mp_obj_get_int(args[1]);
        if (value < -32768 || value > 32767) {
            mp_raise_ValueError(MP_ERROR_TEXT("cycles out of range"));
        }
        mp_uint_t irq_state = MICROPY_BEGIN_ATOMIC_SECTION();
        int16_t prev = machine_counter_cycles_get(self);
        self->cycles_u16 = (uint16_t)(int16_t)value;
        MICROPY_END_ATOMIC_SECTION(irq_state);
        return mp_obj_new_int(prev);
    }

    // Return current cycles value when no argument is provided.
    mp_uint_t irq_state = MICROPY_BEGIN_ATOMIC_SECTION();
    int16_t prev = machine_counter_cycles_get(self);
    MICROPY_END_ATOMIC_SECTION(irq_state);

    return mp_obj_new_int(prev);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_counter_cycles_obj, 1, 2, machine_counter_cycles);

// ---------------------------------------------------------------------------
// counter.irq(handler=None, trigger=0, hard=False)
// ---------------------------------------------------------------------------
// Return or configure a Counter IRQ object for this counter.
static mp_obj_t machine_counter_irq(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    machine_counter_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    bool any_args = n_args > 1 || kw_args->used != 0;

    if (!self->configured) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialised"));
    }

    if (self->mp_irq_obj == NULL) {
        self->mp_irq_obj = mp_irq_new(&machine_counter_irq_methods, MP_OBJ_FROM_PTR(self));
        self->mp_irq_obj->ishard = false;
    }

    if (any_args) {
        mp_arg_val_t args[MP_IRQ_ARG_INIT_NUM_ARGS];
        mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
            MP_IRQ_ARG_INIT_NUM_ARGS, mp_irq_init_args, args);

        mp_obj_t handler = args[MP_IRQ_ARG_INIT_handler].u_obj;
        if (handler != mp_const_none && !mp_obj_is_callable(handler)) {
            mp_raise_ValueError(MP_ERROR_TEXT("handler must be None or callable"));
        }

        mp_uint_t trigger = args[MP_IRQ_ARG_INIT_trigger].u_int;
        mp_uint_t not_supported = trigger & ~MACHINE_COUNTER_ALLOWED_FLAGS;
        if (trigger != 0 && not_supported) {
            mp_raise_msg_varg(&mp_type_ValueError,
                MP_ERROR_TEXT("trigger 0x%08x unsupported"), not_supported);
        }

        self->mp_irq_obj->handler = handler;
        self->mp_irq_obj->ishard = args[MP_IRQ_ARG_INIT_hard].u_bool;

        self->mp_irq_flags = 0;
        self->mp_irq_trigger = trigger;

        mp_uint_t irq_state = MICROPY_BEGIN_ATOMIC_SECTION();
        machine_counter_apply_interrupt_mask(self);
        MICROPY_END_ATOMIC_SECTION(irq_state);
    }

    return MP_OBJ_FROM_PTR(self->mp_irq_obj);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_counter_irq_obj, 1, machine_counter_irq);

// ---------------------------------------------------------------------------
// Printing
// ---------------------------------------------------------------------------
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
    { MP_ROM_QSTR(MP_QSTR_cycles), MP_ROM_PTR(&machine_counter_cycles_obj) },
    { MP_ROM_QSTR(MP_QSTR_irq), MP_ROM_PTR(&machine_counter_irq_obj) },

    { MP_ROM_QSTR(MP_QSTR_IRQ_RESET), MP_ROM_INT(MACHINE_COUNTER_IRQ_RESET) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_INDEX), MP_ROM_INT(MACHINE_COUNTER_IRQ_INDEX) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_MATCH), MP_ROM_INT(MACHINE_COUNTER_IRQ_MATCH) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_ROLL_UNDER), MP_ROM_INT(MACHINE_COUNTER_IRQ_ROLL_UNDER) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_ROLL_OVER), MP_ROM_INT(MACHINE_COUNTER_IRQ_ROLL_OVER) },

    { MP_ROM_QSTR(MP_QSTR_RISING), MP_ROM_INT(COUNTER_EDGE_RISING) },
    { MP_ROM_QSTR(MP_QSTR_FALLING), MP_ROM_INT(COUNTER_EDGE_FALLING) },
    { MP_ROM_QSTR(MP_QSTR_UP), MP_ROM_INT(COUNTER_DIR_UP) },
    { MP_ROM_QSTR(MP_QSTR_DOWN), MP_ROM_INT(COUNTER_DIR_DOWN) },
};
static MP_DEFINE_CONST_DICT(machine_counter_locals_dict, machine_counter_locals_dict_table);

// ---------------------------------------------------------------------------
// Type Registration
// ---------------------------------------------------------------------------

MP_DEFINE_CONST_OBJ_TYPE(
    machine_counter_type,
    MP_QSTR_Counter,
    MP_TYPE_FLAG_NONE,
    make_new, machine_counter_make_new,
    print, machine_counter_print,
    locals_dict, &machine_counter_locals_dict
    );
