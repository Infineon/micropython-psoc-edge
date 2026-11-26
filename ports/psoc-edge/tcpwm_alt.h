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

#ifndef MICROPY_INCLUDED_PSOC_EDGE_TCPWM_H
#define MICROPY_INCLUDED_PSOC_EDGE_TCPWM_H

#include "sys_int.h"
#include "py/obj.h"
#include "clk.h"

typedef void (*tcpwm_owner_irq_handler_t)(mp_obj_t tcpwm_obj);

typedef struct _tcpwm_obj_t {
    int16_t id;
    TCPWM_Type *tcpwm;
    pclk_div_obj_t *pclk_div;
    sys_int_cfg_t irq;
    en_clk_dst_t clk;
    mp_obj_t owner;
    tcpwm_owner_irq_handler_t owner_handler;
} tcpwm_obj_t;

void tcpwm_id_assert(uint16_t id);
uint32_t tcpwm_get_max_period(uint16_t id);

tcpwm_obj_t *tcpwm_obj_alloc(uint16_t id, mp_obj_t owner, tcpwm_owner_irq_handler_t handler);
void tcpwm_obj_free(tcpwm_obj_t *tcpwm);

void tcpwm_clock_init(tcpwm_obj_t *obj, uint32_t target_hz);
void tcpwm_clock_deinit(tcpwm_obj_t *obj);

#endif // MICROPY_INCLUDED_PSOC_EDGE_TCPWM_H
