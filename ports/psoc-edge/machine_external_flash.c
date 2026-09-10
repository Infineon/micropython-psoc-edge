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

#include "py/runtime.h"
#include "py/misc.h"

#include "cybsp.h"
#include "cy_pdl.h"
#include "cycfg_qspi_memslot.h"
#include "mtb_serial_memory.h"

#define EXTERNAL_FLASH_BASE (EXT_FLASH_SHARED_DATA_BASE)
#define EXTERNAL_FLASH_SECTOR_SIZE (0x00040000UL)
#define EXTERNAL_FLASH_METADATA_BASE (EXT_FLASH_SHARED_DATA_BASE + EXT_FLASH_SHARED_DATA_SIZE - EXTERNAL_FLASH_SECTOR_SIZE)
#define EXTERNAL_FLASH_SIZE (EXT_FLASH_SHARED_DATA_SIZE - EXTERNAL_FLASH_SECTOR_SIZE)
#define EXTERNAL_FLASH_METADATA_MAGIC (0x58414441UL)

// ExternalFlash offsets are relative to the shared-data region. The final
// sector is reserved for persistent allocation metadata.

typedef struct {
    uint32_t magic;
    uint32_t offset;
    uint32_t length;
    uint32_t reserved;
} machine_external_flash_allocation_t;

extern uint8_t qspi_flash_init;
extern mtb_serial_memory_t serial_memory_obj;
extern cy_stc_smif_mem_context_t smif_mem_context;
extern cy_stc_smif_mem_info_t smif_mem_info;

typedef struct {
    mp_obj_base_t base;
} machine_external_flash_obj_t;

static machine_external_flash_obj_t machine_external_flash_obj;

static void machine_external_flash_init(void) {
    if (!qspi_flash_init) {
        cy_rslt_t result = mtb_serial_memory_setup(&serial_memory_obj,
            MTB_SERIAL_MEMORY_CHIP_SELECT_1,
            CYBSP_SMIF_CORE_0_XSPI_FLASH_hal_config.base,
            CYBSP_SMIF_CORE_0_XSPI_FLASH_hal_config.clock,
            &smif_mem_context,
            &smif_mem_info,
            &smif0BlockConfig);
        if (result != CY_RSLT_SUCCESS) {
            mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("flash init failed: 0x%08lx"), result);
        }
        qspi_flash_init = true;
    }
}

static void machine_external_flash_check_range(uint32_t offset, size_t len) {
    if (offset > EXTERNAL_FLASH_SIZE || len > EXTERNAL_FLASH_SIZE - offset) {
        mp_raise_ValueError(MP_ERROR_TEXT("flash range out of bounds"));
    }
}

// Scan the append-only allocation log and return the high-water mark.
static uint32_t machine_external_flash_scan_allocations(size_t *next_record) {
    uint32_t used = 0;
    size_t record = 0;
    size_t record_count = EXTERNAL_FLASH_SECTOR_SIZE / sizeof(machine_external_flash_allocation_t);
    machine_external_flash_allocation_t allocation;

    for (; record < record_count; ++record) {
        cy_rslt_t result = mtb_serial_memory_read(&serial_memory_obj,
            EXTERNAL_FLASH_METADATA_BASE + record * sizeof(allocation), sizeof(allocation), (uint8_t *)&allocation);
        if (result != CY_RSLT_SUCCESS) {
            mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("allocation metadata read failed: 0x%08lx"), result);
        }
        if (allocation.magic == 0xffffffffUL) {
            break;
        }
        if (allocation.magic != EXTERNAL_FLASH_METADATA_MAGIC || allocation.length == 0 || allocation.offset > EXTERNAL_FLASH_SIZE || allocation.length > EXTERNAL_FLASH_SIZE - allocation.offset) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("invalid allocation metadata"));
        }
        if (allocation.offset + allocation.length > used) {
            used = allocation.offset + allocation.length;
        }
    }
    if (next_record != NULL) {
        *next_record = record;
    }
    return used;
}

// machine_external_flash_make_new - Create a new instance of the external flash object
static mp_obj_t machine_external_flash_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    (void)args;
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    machine_external_flash_init();
    machine_external_flash_obj.base.type = type;
    return MP_OBJ_FROM_PTR(&machine_external_flash_obj);
}

// init() - Initialize the external flash interface
static mp_obj_t machine_external_flash_init_method(mp_obj_t self_in) {
    (void)self_in;
    machine_external_flash_init();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_external_flash_init_method_obj, machine_external_flash_init_method);

// read() - Read data from external flash memory
static mp_obj_t machine_external_flash_read(mp_obj_t self_in, mp_obj_t offset_in, mp_obj_t len_in) {
    (void)self_in;
    mp_int_t offset = mp_obj_get_int(offset_in);
    mp_int_t len = mp_obj_get_int(len_in);
    if (offset < 0 || len < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid flash range"));
    }
    machine_external_flash_check_range((uint32_t)offset, (size_t)len);

    byte *buf = m_new(byte, len);
    cy_rslt_t result = mtb_serial_memory_read(&serial_memory_obj, EXTERNAL_FLASH_BASE + (uint32_t)offset, len, buf);
    if (result != CY_RSLT_SUCCESS) {
        m_del(byte, buf, len);
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("flash read failed: 0x%08lx"), result);
    }
    mp_obj_t result_obj = mp_obj_new_bytes(buf, len);
    m_del(byte, buf, len);
    return result_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_3(machine_external_flash_read_obj, machine_external_flash_read);

// write() - Write previously erased flash pages. Call erase() before reusing sectors.
static mp_obj_t machine_external_flash_write(mp_obj_t self_in, mp_obj_t offset_in, mp_obj_t data_in) {
    (void)self_in;
    mp_int_t offset = mp_obj_get_int(offset_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    if (offset < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid flash offset"));
    }
    machine_external_flash_check_range((uint32_t)offset, bufinfo.len);

    cy_rslt_t result = mtb_serial_memory_write(&serial_memory_obj,
        EXTERNAL_FLASH_BASE + (uint32_t)offset, bufinfo.len, bufinfo.buf);
    if (result != CY_RSLT_SUCCESS) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("flash write failed: 0x%08lx"), result);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(machine_external_flash_write_obj, machine_external_flash_write);

// erase() - Erase flash sectors starting at the specified offset until the specified length. The offset and length must be sector aligned.
static mp_obj_t machine_external_flash_erase(mp_obj_t self_in, mp_obj_t offset_in, mp_obj_t len_in) {
    (void)self_in;
    mp_int_t offset = mp_obj_get_int(offset_in);
    mp_int_t len = mp_obj_get_int(len_in);
    if (offset < 0 || len < 0 || (uint32_t)offset % EXTERNAL_FLASH_SECTOR_SIZE != 0 || (uint32_t)len % EXTERNAL_FLASH_SECTOR_SIZE != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("flash erases must be sector aligned"));
    }
    machine_external_flash_check_range((uint32_t)offset, (size_t)len);

    for (size_t erased = 0; erased < (size_t)len; erased += EXTERNAL_FLASH_SECTOR_SIZE) {
        uint32_t address = EXTERNAL_FLASH_BASE + (uint32_t)offset + erased;
        cy_rslt_t result = mtb_serial_memory_erase(&serial_memory_obj, address,
            mtb_serial_memory_get_erase_size(&serial_memory_obj, address));
        if (result != CY_RSLT_SUCCESS) {
            mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("flash erase failed: 0x%08lx"), result);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(machine_external_flash_erase_obj, machine_external_flash_erase);

// allocate() - Allocate persistent sector-aligned storage and return its relative offset.
static mp_obj_t machine_external_flash_allocate(mp_obj_t self_in, mp_obj_t length_in) {
    (void)self_in;
    mp_int_t requested_length = mp_obj_get_int(length_in);
    if (requested_length <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("allocation length must be positive"));
    }
    if ((uint64_t)requested_length > EXTERNAL_FLASH_SIZE) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("insufficient shared flash space: %lu bytes free"), EXTERNAL_FLASH_SIZE);
    }

    uint32_t length = ((uint32_t)requested_length + EXTERNAL_FLASH_SECTOR_SIZE - 1) & ~(EXTERNAL_FLASH_SECTOR_SIZE - 1);

    size_t next_record;
    uint32_t used = machine_external_flash_scan_allocations(&next_record);
    uint32_t offset = (used + EXTERNAL_FLASH_SECTOR_SIZE - 1) & ~(EXTERNAL_FLASH_SECTOR_SIZE - 1);
    if (length > EXTERNAL_FLASH_SIZE - offset) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("insufficient shared flash space: %lu bytes free"), EXTERNAL_FLASH_SIZE - offset);
    }
    if (next_record >= EXTERNAL_FLASH_SECTOR_SIZE / sizeof(machine_external_flash_allocation_t)) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("allocation metadata is full"));
    }

    machine_external_flash_allocation_t allocation = {
        .magic = EXTERNAL_FLASH_METADATA_MAGIC,
        .offset = offset,
        .length = length,
        .reserved = 0xffffffffUL,
    };
    cy_rslt_t result = mtb_serial_memory_write(&serial_memory_obj,
        EXTERNAL_FLASH_METADATA_BASE + next_record * sizeof(allocation), sizeof(allocation), (const uint8_t *)&allocation);
    if (result != CY_RSLT_SUCCESS) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("allocation metadata write failed: 0x%08lx"), result);
    }
    return mp_obj_new_int(offset);
}
static MP_DEFINE_CONST_FUN_OBJ_2(machine_external_flash_allocate_obj, machine_external_flash_allocate);

// used() - Return the total number of bytes used in the external flash.
static mp_obj_t machine_external_flash_used(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_int(machine_external_flash_scan_allocations(NULL));
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_external_flash_used_obj, machine_external_flash_used);
// free() - Return the total number of free bytes in the external flash.
static mp_obj_t machine_external_flash_free(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_int(EXTERNAL_FLASH_SIZE - machine_external_flash_scan_allocations(NULL));
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_external_flash_free_obj, machine_external_flash_free);

// capacity() - Return the total user-addressable shared-data capacity in bytes.
static mp_obj_t machine_external_flash_capacity(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_int(EXTERNAL_FLASH_SIZE);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_external_flash_capacity_obj, machine_external_flash_capacity);

// sector_size() - Return the size of an erase sector in bytes.
static mp_obj_t machine_external_flash_sector_size(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_int(EXTERNAL_FLASH_SECTOR_SIZE);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_external_flash_sector_size_obj, machine_external_flash_sector_size);

// info() - Return capacity, erase-sector size, allocation , and free space.
static mp_obj_t machine_external_flash_info(mp_obj_t self_in) {
    (void)self_in;
    mp_obj_t info = mp_obj_new_dict(2);
    mp_obj_dict_store(info, MP_OBJ_NEW_QSTR(MP_QSTR_capacity), mp_obj_new_int(EXTERNAL_FLASH_SIZE));
    mp_obj_dict_store(info, MP_OBJ_NEW_QSTR(MP_QSTR_sector_size), mp_obj_new_int(EXTERNAL_FLASH_SECTOR_SIZE));
    mp_obj_dict_store(info, MP_OBJ_NEW_QSTR(MP_QSTR_used), mp_obj_new_int(machine_external_flash_scan_allocations(NULL)));
    mp_obj_dict_store(info, MP_OBJ_NEW_QSTR(MP_QSTR_free), mp_obj_new_int(EXTERNAL_FLASH_SIZE - machine_external_flash_scan_allocations(NULL)));
    return info;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_external_flash_info_obj, machine_external_flash_info);

static const mp_rom_map_elem_t machine_external_flash_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&machine_external_flash_init_method_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&machine_external_flash_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&machine_external_flash_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_erase), MP_ROM_PTR(&machine_external_flash_erase_obj) },
    { MP_ROM_QSTR(MP_QSTR_allocate), MP_ROM_PTR(&machine_external_flash_allocate_obj) },
    { MP_ROM_QSTR(MP_QSTR_used), MP_ROM_PTR(&machine_external_flash_used_obj) },
    { MP_ROM_QSTR(MP_QSTR_free), MP_ROM_PTR(&machine_external_flash_free_obj) },
    { MP_ROM_QSTR(MP_QSTR_capacity), MP_ROM_PTR(&machine_external_flash_capacity_obj) },
    { MP_ROM_QSTR(MP_QSTR_sector_size), MP_ROM_PTR(&machine_external_flash_sector_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&machine_external_flash_info_obj) },
};
static MP_DEFINE_CONST_DICT(machine_external_flash_locals_dict, machine_external_flash_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_external_flash_type,
    MP_QSTR_ExternalFlash,
    MP_TYPE_FLAG_NONE,
    make_new, machine_external_flash_make_new,
    locals_dict, &machine_external_flash_locals_dict
    );
