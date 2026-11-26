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
#include "shared/runtime/mpirq.h"
#include "sys_int.h"

#include "cy_tcpwm_counter.h"
#include "tcpwm_alt.h"
#include "genhdr/pins_af.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Shared general-purpose timer divider target frequency used by this module.
#define TIMER_CLK_HZ  (1000000UL)

// Timer mode values — also exposed as Python class constants
#define TIMER_MODE_ONE_SHOT  (1)
#define TIMER_MODE_PERIODIC  (2)

// ---------------------------------------------------------------------------
// Object type
// ---------------------------------------------------------------------------

typedef struct _machine_timer_obj_t {
    mp_obj_base_t base;
    int id;                  // id matches TCPWM0 counter id (0-7 for Group 0 32-bit counters, 256-263 for Group 1 16-bit counters)
    tcpwm_obj_t *tcpwm;
    uint8_t mode;
    uint32_t period_ticks;
    mp_obj_t callback;
    bool ishard;
} machine_timer_obj_t;

const mp_obj_type_t machine_timer_type;  // forward declaration

#define MICROPY_PY_MACHINE_TIMER_NUM_ENTRIES (MICROPY_PY_MACHINE_TCPWM0_16BITS_NUM_COUNTERS + MICROPY_PY_MACHINE_TCPWM0_32BITS_NUM_COUNTERS)

// One Timer object per id, kept alive by the GC as a root pointer.
MP_REGISTER_ROOT_POINTER(struct _machine_timer_obj_t *machine_timer_obj[MICROPY_PY_MACHINE_TIMER_NUM_ENTRIES]);

// ---------------------------------------------------------------------------
// IRQ handling helpers
// ---------------------------------------------------------------------------

static void machine_timer_isr(mp_obj_t timer_obj) {
    machine_timer_obj_t *self = MP_OBJ_TO_PTR(timer_obj);

    if (Cy_TCPWM_GetInterruptStatus(self->tcpwm->tcpwm, self->tcpwm->id) & CY_TCPWM_INT_ON_TC) {
        Cy_TCPWM_ClearInterrupt(self->tcpwm->tcpwm, self->tcpwm->id, CY_TCPWM_INT_ON_TC);
        if (self->mode == TIMER_MODE_ONE_SHOT) {
            Cy_TCPWM_Counter_Disable(self->tcpwm->tcpwm, self->tcpwm->id);
        }
        if (self->callback != mp_const_none
            && mp_irq_dispatch(self->callback, MP_OBJ_FROM_PTR(self), self->ishard) < 0) {
            // Uncaught exception: stop timer and disable callback to avoid repeated IRQ spam.
            Cy_TCPWM_Counter_Disable(self->tcpwm->tcpwm, self->tcpwm->id);
            self->callback = mp_const_none;
        }
    }
}

// ---------------------------------------------------------------------------
// Hardware helpers
// ---------------------------------------------------------------------------

static machine_timer_obj_t *mp_machine_timer_obj_get(uint16_t id) {
    for (uint8_t i = 0; i < MICROPY_PY_MACHINE_TIMER_NUM_ENTRIES; i++) {
        if (MP_STATE_PORT(machine_timer_obj[i]) != NULL) {
            if (MP_STATE_PORT(machine_timer_obj[i])->id == id) {
                return MP_STATE_PORT(machine_timer_obj[i]);
            }
        }
    }
    return NULL;
}

static machine_timer_obj_t *mp_machine_timer_obj_alloc(void) {
    machine_timer_obj_t *self = NULL;
    for (uint8_t i = 0; i < MICROPY_PY_MACHINE_TIMER_NUM_ENTRIES; i++) {
        if (MP_STATE_PORT(machine_timer_obj[i]) == NULL) {
            self = mp_obj_malloc(machine_timer_obj_t, &machine_timer_type);
            MP_STATE_PORT(machine_timer_obj[i]) = self;
            self->tcpwm = NULL;
            break;
        }
    }

    if (self == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("all available Timer instances are allocated"));
    }

    return self;
}

static void mp_machine_timer_obj_free(machine_timer_obj_t *self) {
    for (uint8_t i = 0; i < MICROPY_PY_MACHINE_TIMER_NUM_ENTRIES; i++) {
        if (MP_STATE_PORT(machine_timer_obj[i]) == self) {
            MP_STATE_PORT(machine_timer_obj[i]) = NULL;
            break;
        }
    }
}

static void machine_timer_hw_init(machine_timer_obj_t *self) {
    // Input-pin disabled sentinel (upper 2 bits are ignored; mask to 2-bit field)
    #define TIMER_INPUT_DISABLED  (0x7U)

    cy_stc_tcpwm_counter_config_t cfg = {
        .period = self->period_ticks - 1U,   // The counter counts 0 → period_reg (inclusive) then fires TC. So period_reg = period_ticks - 1.
        .clockPrescaler = CY_TCPWM_COUNTER_PRESCALER_DIVBY_1,
        .runMode = (self->mode == TIMER_MODE_ONE_SHOT)
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

    if (self->tcpwm == NULL) {
        self->tcpwm = tcpwm_obj_alloc(self->id, self, machine_timer_isr);
    }
    tcpwm_clock_init(self->tcpwm, TIMER_CLK_HZ);

    cy_en_tcpwm_status_t result = Cy_TCPWM_Counter_Init(self->tcpwm->tcpwm, self->tcpwm->id, &cfg);
    if (result != CY_TCPWM_SUCCESS) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("Timer(%u) init failed (PDL error 0x%lx)"),
            (unsigned)self->id, (unsigned long)result);
    }
    Cy_TCPWM_ClearInterrupt(self->tcpwm->tcpwm, self->tcpwm->id, CY_TCPWM_INT_ON_TC);

    sys_int_init(&self->tcpwm->irq);

    Cy_TCPWM_Counter_Enable(self->tcpwm->tcpwm, self->tcpwm->id);
    Cy_TCPWM_TriggerReloadOrIndex_Single(self->tcpwm->tcpwm, self->tcpwm->id);
}

static void machine_timer_hw_deinit(machine_timer_obj_t *self) {
    if (self->tcpwm != NULL) {
        Cy_TCPWM_Counter_Disable(self->tcpwm->tcpwm, self->tcpwm->id);
        sys_int_deinit(&self->tcpwm->irq);
        tcpwm_clock_deinit(self->tcpwm);
        tcpwm_obj_free(self->tcpwm);
        self->tcpwm = NULL;
    }
}

static void machine_timer_init_impl(machine_timer_obj_t *self, int timer_id, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_mode, ARG_callback, ARG_period, ARG_freq, ARG_hard };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = TIMER_MODE_PERIODIC} },
        { MP_QSTR_callback, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_period,   MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_freq,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_hard,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_FALSE} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    /* -- Mode -- */
    mp_int_t mode = args[ARG_mode].u_int;
    if (mode != TIMER_MODE_ONE_SHOT && mode != TIMER_MODE_PERIODIC) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
    }

    /* -- Callback -- */
    mp_obj_t callback = args[ARG_callback].u_obj;
    if (callback != mp_const_none && !mp_obj_is_callable(callback)) {
        mp_raise_ValueError(MP_ERROR_TEXT("callback must be callable"));
    }

    /* -- Hard -- */
    mp_obj_t hard = args[ARG_hard].u_obj;
    if (!mp_obj_is_bool(hard)) {
        mp_raise_ValueError(MP_ERROR_TEXT("hard must be bool"));
    }

    /* -- Period / Frequency -- */
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
    uint32_t period_max = tcpwm_get_max_period(timer_id);
    if (period_ticks_64 > period_max) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("16-bit Timer(%u) period exceeds %lu ticks; use a shorter period/higher freq, or use a 32-bit Timer (id 0-7)"),
            (unsigned)timer_id,
            (unsigned long)period_max);
    }

    /* -- Reinitialization reset -- */
    machine_timer_hw_deinit(self);

    /* -- Timer param init*/
    self->mode = mode;
    self->period_ticks = (uint32_t)period_ticks_64;
    self->callback = callback;
    self->ishard = hard;

    /* -- Initialise hardware -- */
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        machine_timer_hw_init(self);
        nlr_pop();
    } else {
        // Ensure partially-initialized instances are fully released on init failure.
        machine_timer_hw_deinit(self);
        nlr_raise(nlr.ret_val);
    }
}

// ---------------------------------------------------------------------------
// timer.deinit()
// ---------------------------------------------------------------------------

static mp_obj_t machine_timer_deinit(mp_obj_t self_in) {
    machine_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    machine_timer_hw_deinit(self);
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

    int tcpwm_id = -1;
    size_t init_n_args = n_args;
    const mp_obj_t *init_args = args;
    if (n_args > 0 && mp_obj_is_int(args[0])) {
        tcpwm_id = mp_obj_get_int(args[0]);
        /* TODO: Handle -1 case.
        May we if a -1 is used, just take any timer available? */
        tcpwm_id_assert(tcpwm_id);
        init_n_args = n_args - 1;
        init_args = args + 1;
    } else if (n_args > 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("Timer id must be an integer"));
    }

    machine_timer_obj_t *self = mp_machine_timer_obj_get(tcpwm_id);
    bool is_new = (self == NULL);
    if (is_new) {
        self = mp_machine_timer_obj_alloc();
        self->id = tcpwm_id;
    }

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);

    if (init_n_args > 0 || n_kw > 0) {
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            machine_timer_init_impl(self, tcpwm_id, init_n_args, init_args, &kw_args);
            nlr_pop();
        } else {
            // A failed construction of an existing Timer must leave it untouched.
            if (is_new) {
                machine_timer_deinit(MP_OBJ_FROM_PTR(self));
                mp_machine_timer_obj_free(self);
            }
            nlr_raise(nlr.ret_val);
        }
    }

    return MP_OBJ_FROM_PTR(self);
}

// ---------------------------------------------------------------------------
// timer.init(mode=, freq=, period=, callback=, hard=)
// ---------------------------------------------------------------------------

static mp_obj_t machine_timer_init(size_t n_args, const mp_obj_t *args,
    mp_map_t *kw_args) {
    machine_timer_obj_t *self_in = MP_OBJ_TO_PTR(args[0]);
    machine_timer_init_impl(self_in, self_in->id, n_args - 1, args + 1, kw_args);
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
    for (uint8_t i = 0; i < MICROPY_PY_MACHINE_TIMER_NUM_ENTRIES; i++) {
        machine_timer_obj_t *self = MP_STATE_PORT(machine_timer_obj[i]);
        if (self != NULL) {
            machine_timer_deinit(MP_OBJ_FROM_PTR(self));
            mp_machine_timer_obj_free(self);
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
