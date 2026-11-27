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

// Hardware timer using TCPWM0 counters.
// IDs 0..7 are Group 0 (32-bit) counters.
// IDs 8..31 are Group 1 (16-bit) counters.

#include "py/obj.h"
#include "py/runtime.h"
#include "modmachine.h"
#include "shared/runtime/mpirq.h"
#include "sys_int.h"

#include "clk.h"
#include "cy_tcpwm_counter.h"
#include "cy_tcpwm.h"
#include "tcpwm.h"
#include "genhdr/pins_af.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define MACHINE_TIMER_NUM_INSTANCES  MICROPY_PY_MACHINE_TCPWM0_NUM_COUNTERS

// Shared general-purpose timer divider target frequency used by this module.
#define TIMER_CLK_HZ  (1000000UL)

// Input-pin disabled sentinel (upper 2 bits are ignored; mask to 2-bit field)
#define TIMER_INPUT_DISABLED  (0x7U)

// Timer mode values — also exposed as Python class constants
#define TIMER_MODE_ONE_SHOT  (1)
#define TIMER_MODE_PERIODIC  (2)

// ---------------------------------------------------------------------------
// Object type
// ---------------------------------------------------------------------------

typedef struct _machine_timer_obj_t {
    mp_obj_base_t base;
    uint8_t id;                  // 0..31
    uint8_t mode;                // TIMER_MODE_ONE_SHOT or TIMER_MODE_PERIODIC
    bool ishard;
    bool irq_active;             // counter IRQ registered with sys_int
    uint32_t counter_num;        // TCPWM0 counter number for this id
    uint32_t period_ticks;       // configured period in timer clock ticks, 0 if not started
    en_clk_dst_t pclk_dst;
    pclk_div_obj_t *pclk_div;
    mp_obj_t callback;
    sys_int_cfg_t irq_cfg;
} machine_timer_obj_t;

const mp_obj_type_t machine_timer_type;  // forward declaration

// One Timer object per id, kept alive by the GC as a root pointer.
MP_REGISTER_ROOT_POINTER(struct _machine_timer_obj_t *machine_timer_obj[MICROPY_PY_MACHINE_TCPWM0_NUM_COUNTERS]);

// ---------------------------------------------------------------------------
// IRQ handling helpers
// ---------------------------------------------------------------------------

static void machine_timer_isr(machine_timer_obj_t *self) {
    if (Cy_TCPWM_GetInterruptStatus(TCPWM0, self->counter_num) & CY_TCPWM_INT_ON_TC) {
        Cy_TCPWM_ClearInterrupt(TCPWM0, self->counter_num, CY_TCPWM_INT_ON_TC);
        if (self->mode == TIMER_MODE_ONE_SHOT) {
            Cy_TCPWM_Counter_Disable(TCPWM0, self->counter_num);
        }
        if (self->callback != mp_const_none
            && mp_irq_dispatch(self->callback, MP_OBJ_FROM_PTR(self), self->ishard) < 0) {
            // Uncaught exception: stop timer and disable callback to avoid repeated IRQ spam.
            Cy_TCPWM_Counter_Disable(TCPWM0, self->counter_num);
            self->callback = mp_const_none;
        }
    }
}

// Generate one IRQ handler per timer id for direct dispatch without scanning.
#define TIMER_IRQ_HANDLER_DECL(idx, grp, cnt) \
    static void machine_timer_irq_handler_##idx(void) { \
        machine_timer_obj_t *self = MP_STATE_PORT(machine_timer_obj[idx]); \
        if (self != NULL) { \
            machine_timer_isr(self); \
        } \
    }
MICROPY_PY_MACHINE_TCPWM0_FOR_ALL_COUNTERS(TIMER_IRQ_HANDLER_DECL)
#undef TIMER_IRQ_HANDLER_DECL

#define TIMER_IRQ_HANDLER_ENTRY(idx, grp, cnt) [idx] = machine_timer_irq_handler_##idx,
static const cy_israddress machine_timer_irq_handlers[MACHINE_TIMER_NUM_INSTANCES] = {
    MICROPY_PY_MACHINE_TCPWM0_FOR_ALL_COUNTERS(TIMER_IRQ_HANDLER_ENTRY)
};
#undef TIMER_IRQ_HANDLER_ENTRY

// ---------------------------------------------------------------------------
// Hardware helpers
// ---------------------------------------------------------------------------

// Claims the TCPWM0 counter and the id slot for this object. Safe to call
// repeatedly, and raises if another object owns the counter.
static void machine_timer_claim(machine_timer_obj_t *self) {
    machine_tcpwm_counter_alloc(self->counter_num, MP_OBJ_FROM_PTR(self));
    MP_STATE_PORT(machine_timer_obj[self->id]) = self;
}

// Stops the counter and unregisters its IRQ.
static void machine_timer_stop(machine_timer_obj_t *self) {
    // Only touch the TCPWM0 block if this timer has its clock configured.
    if (self->pclk_div != NULL) {
        Cy_TCPWM_Counter_Disable(TCPWM0, self->counter_num);
    }
    if (self->irq_active) {
        sys_int_deinit(&self->irq_cfg);
        self->irq_active = false;
    }
    self->period_ticks = 0;
}

// Applies a validated configuration and (re)starts the counter.
static void machine_timer_start(machine_timer_obj_t *self, uint8_t mode,
    mp_obj_t callback, bool ishard, uint32_t period_ticks) {

    // Stop the previous run before the new settings are visible to the ISR.
    machine_timer_stop(self);
    self->mode = mode;
    self->callback = callback;
    self->ishard = ishard;

    if (self->pclk_div == NULL) {
        self->pclk_div = machine_tcpwm_clock_init(self->pclk_dst, TIMER_CLK_HZ);
    }

    // The counter counts 0 → period_reg (inclusive) then fires TC.
    // So period_reg = period_ticks - 1.
    cy_stc_tcpwm_counter_config_t cfg = {
        .period = period_ticks - 1U,
        .clockPrescaler = CY_TCPWM_COUNTER_PRESCALER_DIVBY_1,
        .runMode = (mode == TIMER_MODE_ONE_SHOT)
                              ? CY_TCPWM_COUNTER_ONESHOT
                              : CY_TCPWM_COUNTER_CONTINUOUS,
        .countDirection = CY_TCPWM_COUNTER_COUNT_UP,
        .compareOrCapture = CY_TCPWM_COUNTER_MODE_COMPARE,
        .compare0 = 0U,
        .compare1 = 0U,
        .enableCompareSwap = false,
        .interruptSources = CY_TCPWM_INT_ON_TC,
        // All trigger inputs disabled
        .captureInputMode = TIMER_INPUT_DISABLED & 0x3U,
        .captureInput = CY_TCPWM_INPUT_0,
        .reloadInputMode = TIMER_INPUT_DISABLED & 0x3U,
        .reloadInput = CY_TCPWM_INPUT_0,
        .startInputMode = TIMER_INPUT_DISABLED & 0x3U,
        .startInput = CY_TCPWM_INPUT_0,
        .stopInputMode = TIMER_INPUT_DISABLED & 0x3U,
        .stopInput = CY_TCPWM_INPUT_0,
        .countInputMode = TIMER_INPUT_DISABLED & 0x3U,
        .countInput = CY_TCPWM_INPUT_1,
        .capture1InputMode = TIMER_INPUT_DISABLED & 0x3U,
        .capture1Input = CY_TCPWM_INPUT_0,
        .enableCompare1Swap = false,
        .compare2 = CY_TCPWM_GRP_CNT_CC0_DEFAULT,
        .compare3 = CY_TCPWM_GRP_CNT_CC0_BUFF_DEFAULT,
        .trigger0Event = CY_TCPWM_CNT_TRIGGER_ON_DISABLED,
        .trigger1Event = CY_TCPWM_CNT_TRIGGER_ON_DISABLED,
    };

    cy_en_tcpwm_status_t result = Cy_TCPWM_Counter_Init(TCPWM0, self->counter_num, &cfg);
    if (result != CY_TCPWM_SUCCESS) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("Timer(%u) init failed (PDL error 0x%lx)"),
            (unsigned)self->id, (unsigned long)result);
    }
    // Drop a TC left pending by the previous run, so the new callback is not called early.
    Cy_TCPWM_ClearInterrupt(TCPWM0, self->counter_num, CY_TCPWM_INT_ON_TC);

    // Register IRQ before enabling so no TC edge is missed.
    self->irq_cfg.priority = SYS_INT_IRQ_LOWEST_PRIORITY;
    sys_int_init(&self->irq_cfg);
    self->irq_active = true;

    self->period_ticks = period_ticks;
    Cy_TCPWM_Counter_Enable(TCPWM0, self->counter_num);
    Cy_TCPWM_TriggerReloadOrIndex_Single(TCPWM0, self->counter_num);
}

// ---------------------------------------------------------------------------
// Python init helper (shared by make_new and .init())
// ---------------------------------------------------------------------------

// Validates all the arguments before changing the object, so a failed init()
// leaves a running timer untouched. Arguments not given take their defaults.
static void machine_timer_init_helper(machine_timer_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    enum { ARG_mode, ARG_callback, ARG_period, ARG_freq, ARG_hard };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = TIMER_MODE_PERIODIC} },
        { MP_QSTR_callback, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_period,   MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_freq,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_hard,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_FALSE} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t mode = args[ARG_mode].u_int;
    if (mode != TIMER_MODE_ONE_SHOT && mode != TIMER_MODE_PERIODIC) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
    }

    mp_obj_t callback = args[ARG_callback].u_obj;
    if (callback != mp_const_none && !mp_obj_is_callable(callback)) {
        mp_raise_ValueError(MP_ERROR_TEXT("callback must be callable"));
    }

    mp_obj_t hard = args[ARG_hard].u_obj;
    if (!mp_obj_is_bool(hard)) {
        mp_raise_ValueError(MP_ERROR_TEXT("hard must be bool"));
    }

    // Compute period in timer clock ticks in 64-bit, then range-check.
    uint64_t period_ticks_64;
    if (args[ARG_freq].u_obj != mp_const_none) {
        mp_int_t freq = mp_obj_get_int(args[ARG_freq].u_obj);
        if (freq <= 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("freq must be > 0"));
        }
        period_ticks_64 = (uint64_t)TIMER_CLK_HZ / (uint64_t)(uint32_t)freq;
    } else if (args[ARG_period].u_int > 0) {
        // period is in milliseconds; convert to timer clock ticks (1 MHz base).
        period_ticks_64 = (uint64_t)TIMER_CLK_HZ * (uint64_t)(uint32_t)args[ARG_period].u_int
            / 1000UL;
    } else if (mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_period), MP_MAP_LOOKUP) != NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("period must be > 0"));
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("either freq or period must be specified"));
    }

    if (period_ticks_64 == 0U || period_ticks_64 > UINT32_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("period out of range for 32-bit counter"));
    }

    // Group 1 counters are 16-bit; reject values that would overflow their period register.
    uint32_t period_max = machine_tcpwm_counter_period_max(self->counter_num);
    if (period_ticks_64 > period_max) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("16-bit Timer(%u) period exceeds %lu ticks; use a shorter period/higher freq, or use a 32-bit Timer (id 0-7)"),
            (unsigned)self->id,
            (unsigned long)period_max);
    }

    // Claim again, in case init() is called after deinit().
    machine_timer_claim(self);
    machine_timer_start(self, (uint8_t)mode, callback, hard == mp_const_true, (uint32_t)period_ticks_64);
}

// ---------------------------------------------------------------------------
// timer.deinit()
// ---------------------------------------------------------------------------

static mp_obj_t machine_timer_deinit(mp_obj_t self_in) {
    machine_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    machine_timer_stop(self);
    machine_tcpwm_clock_deinit(&self->pclk_div);
    self->callback = mp_const_none;
    self->ishard = false;
    machine_tcpwm_counter_free(self->counter_num, MP_OBJ_FROM_PTR(self));
    // A stale object must not release a newer Timer created for the same id.
    if (MP_STATE_PORT(machine_timer_obj[self->id]) == self) {
        MP_STATE_PORT(machine_timer_obj[self->id]) = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_timer_deinit_obj, machine_timer_deinit);

// ---------------------------------------------------------------------------
// machine.Timer(id [, mode=, freq=, period=, callback=, hard=])
// ---------------------------------------------------------------------------

// The constructor sets the identity, everything else is delegated to init.
// Constructing an id already in use returns that same object.
static mp_obj_t machine_timer_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {

    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    mp_int_t id = machine_tcpwm_id_from_obj(args[0], MP_QSTR_Timer);

    machine_timer_obj_t *self = MP_STATE_PORT(machine_timer_obj[id]);
    bool is_new_obj = (self == NULL);
    if (is_new_obj) {
        self = mp_obj_malloc(machine_timer_obj_t, &machine_timer_type);
        self->id = (uint8_t)id;
        self->mode = TIMER_MODE_PERIODIC;
        self->ishard = false;
        self->irq_active = false;
        self->counter_num = machine_tcpwm_counter_num(id);
        self->period_ticks = 0;
        self->pclk_dst = machine_tcpwm_counter_pclk(self->counter_num);
        self->pclk_div = NULL;
        self->callback = mp_const_none;
        self->irq_cfg.irq_num = machine_tcpwm_counter_irq(self->counter_num);
        self->irq_cfg.priority = SYS_INT_IRQ_LOWEST_PRIORITY;
        self->irq_cfg.handler = machine_timer_irq_handlers[id];
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        machine_timer_claim(self);
        if (n_args > 1 || n_kw > 0) {
            mp_map_t kw_map;
            mp_map_init_fixed_table(&kw_map, n_kw, args + n_args);
            machine_timer_init_helper(self, n_args - 1, args + 1, &kw_map);
        }
        nlr_pop();
    } else {
        // Only release an object created here. A live object is kept, as on a failed init().
        if (is_new_obj) {
            machine_timer_deinit(MP_OBJ_FROM_PTR(self));
        }
        nlr_jump(nlr.ret_val);
    }

    return MP_OBJ_FROM_PTR(self);
}

// ---------------------------------------------------------------------------
// timer.init(mode=, freq=, period=, callback=, hard=)
// ---------------------------------------------------------------------------

static mp_obj_t machine_timer_init(size_t n_args, const mp_obj_t *args,
    mp_map_t *kw_args) {
    machine_timer_init_helper(MP_OBJ_TO_PTR(args[0]), n_args - 1, args + 1, kw_args);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_timer_init_obj, 1, machine_timer_init);

// ---------------------------------------------------------------------------
// __repr__ / print
// ---------------------------------------------------------------------------

static void machine_timer_print(const mp_print_t *print, mp_obj_t self_in,
    mp_print_kind_t kind) {
    machine_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    // Printed from the stored config: the counter registers may not be clocked.
    if (self->period_ticks == 0U) {
        mp_printf(print, "Timer(id=%u)", (unsigned)self->id);
        return;
    }
    mp_printf(print, "Timer(id=%u, mode=Timer.%s, freq=%u)",
        (unsigned)self->id,
        self->mode == TIMER_MODE_ONE_SHOT ? "OneShot" : "Periodic",
        (unsigned)(TIMER_CLK_HZ / self->period_ticks));
}

// ---------------------------------------------------------------------------
// Module-level lifecycle called from machine_deinit()
// ---------------------------------------------------------------------------

void machine_timer_deinit_all(void) {
    for (uint8_t i = 0; i < MACHINE_TIMER_NUM_INSTANCES; i++) {
        machine_timer_obj_t *self = MP_STATE_PORT(machine_timer_obj[i]);
        if (self != NULL) {
            machine_timer_deinit(MP_OBJ_FROM_PTR(self));
        }
    }
}

// ---------------------------------------------------------------------------
// Type definition
// ---------------------------------------------------------------------------

static const mp_rom_map_elem_t machine_timer_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init),     MP_ROM_PTR(&machine_timer_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),   MP_ROM_PTR(&machine_timer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_ONE_SHOT), MP_ROM_INT(TIMER_MODE_ONE_SHOT) },
    { MP_ROM_QSTR(MP_QSTR_PERIODIC), MP_ROM_INT(TIMER_MODE_PERIODIC) },
};
static MP_DEFINE_CONST_DICT(machine_timer_locals_dict, machine_timer_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_timer_type,
    MP_QSTR_Timer,
    MP_TYPE_FLAG_NONE,
    make_new, machine_timer_make_new,
    print, machine_timer_print,
    locals_dict, &machine_timer_locals_dict
    );
