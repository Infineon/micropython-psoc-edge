/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2020-2021 Damien P. George
 * Copyright (c) 2022-2024 Infineon Technologies AG
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

// micropython includes
#include "py/runtime.h"
#include "extmod/vfs.h"
#include "modpsocedge.h"
#include "mplogger.h"

//MTB includes
#include "cybsp.h"
#include "cy_pdl.h"
#include "mphalport.h"
#include "cycfg_qspi_memslot.h"
// #include "mtb_serial_memory.h"

// Helper function to get external flash configurations
void get_ext_flash_info(void) {
}

static mp_obj_t psoc_edge_qspi_flash_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
        mplogger_print("\nQSPI flash constructor invoked\n");
        return mp_const_none;

}

static mp_obj_t psoc_edge_qspi_flash_readblocks(size_t n_args, const mp_obj_t *args) {
    mplogger_print("\nQSPI flash readblocks called\n");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(psoc_edge_qspi_flash_readblocks_obj, 3, 4, psoc_edge_qspi_flash_readblocks);

static mp_obj_t psoc_edge_qspi_flash_writeblocks(size_t n_args, const mp_obj_t *args) {
    mplogger_print("\nQSPI flash writeblocks called\n");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(psoc_edge_qspi_flash_writeblocks_obj, 3, 4, psoc_edge_qspi_flash_writeblocks);

static mp_obj_t psoc_edge_qspi_flash_ioctl(size_t n_args, const mp_obj_t *args) {
    mplogger_print("\nQSPI flash ioctl called\n");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(psoc_edge_qspi_flash_ioctl_obj, 2, 4, psoc_edge_qspi_flash_ioctl);

static const mp_rom_map_elem_t psoc_edge_qspi_flash_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_readblocks), MP_ROM_PTR(&psoc_edge_qspi_flash_readblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeblocks), MP_ROM_PTR(&psoc_edge_qspi_flash_writeblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl), MP_ROM_PTR(&psoc_edge_qspi_flash_ioctl_obj) },
};
static MP_DEFINE_CONST_DICT(psoc_edge_qspi_flash_locals_dict, psoc_edge_qspi_flash_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    psoc_edge_qspi_flash_type,
    MP_QSTR_QSPI_Flash,
    MP_TYPE_FLAG_NONE,
    make_new, psoc_edge_qspi_flash_make_new,
    locals_dict, &psoc_edge_qspi_flash_locals_dict
    );

