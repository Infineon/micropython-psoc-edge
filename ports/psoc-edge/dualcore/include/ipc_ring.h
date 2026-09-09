/*
 * ipc_ring.h — Lock-free byte-stream ring buffers in shared SRAM.
 *
 * Two rings, one per direction, living in the m33_m55_shared SOCMEM region
 * which is coherently shared between CM33 and CM55.
 * Both directions use 64 KB payload rings, spaced within the region:
 *   host   -> target  (CM33 -> CM55)  at 0x262FC000  (64 KB payload)
 *   target -> host    (CM55 -> CM33)  at 0x2630D000  (64 KB payload)
 *
 * Single-producer / single-consumer: the producer only ever writes `head`,
 * the consumer only ever writes `tail`, so no lock is needed.
 *
 * Copyright (c) 2026 Infineon Technologies AG
 * SPDX-License-Identifier: MIT
 */

#ifndef IPC_RING_H
#define IPC_RING_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "cmsis_compiler.h"   /* __DMB() */

/* Each ipc_ring_t is a 16-byte header followed by `capacity` payload bytes.
 * host->target is placed first; target->host is spaced past the header + the
 * 64 KB host->target payload. */
#define IPC_RING_HOST_TO_TARGET_ADDR   (0x262FC000u)
#define IPC_RING_TARGET_TO_HOST_ADDR   (0x2630D000u)

#define IPC_RING_MAGIC       (0x52494E47u)  /* 'RING' */

#define IPC_RING_H2T_CAPACITY  (65536u)     /* CM33 -> CM55 input  (bytes) */
#define IPC_RING_T2H_CAPACITY  (65536u)     /* CM55 -> CM33 result (bytes) */

/* End of the m33_m55_shared SOCMEM region (0x262FC000 + 0x40000). */
#define IPC_RING_REGION_END    (0x2633C000u)

/*
 * head/tail are free-running byte counters.
 * capacity is stored per ring at init time.
 */
typedef struct {
    uint32_t magic;                             /* IPC_RING_MAGIC once inited  */
    uint32_t capacity;                          /* per-ring;                   */
    volatile uint32_t head;                     /* producer: total bytes in    */
    volatile uint32_t tail;                     /* consumer: total bytes out   */
    uint8_t data[];                             /* `capacity` bytes follow     */
} ipc_ring_t;

#define IPC_RING_HOST_TO_TARGET  ((ipc_ring_t *)IPC_RING_HOST_TO_TARGET_ADDR)
#define IPC_RING_TARGET_TO_HOST  ((ipc_ring_t *)IPC_RING_TARGET_TO_HOST_ADDR)

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert((IPC_RING_H2T_CAPACITY & (IPC_RING_H2T_CAPACITY - 1u)) == 0u,
    "IPC_RING_H2T_CAPACITY must be a power of two");
_Static_assert((IPC_RING_T2H_CAPACITY & (IPC_RING_T2H_CAPACITY - 1u)) == 0u,
    "IPC_RING_T2H_CAPACITY must be a power of two");
_Static_assert(IPC_RING_TARGET_TO_HOST_ADDR - IPC_RING_HOST_TO_TARGET_ADDR
    >= sizeof(ipc_ring_t) + IPC_RING_H2T_CAPACITY,
    "rings overlap: increase spacing or reduce IPC_RING_H2T_CAPACITY");
_Static_assert(IPC_RING_TARGET_TO_HOST_ADDR + sizeof(ipc_ring_t)
    + IPC_RING_T2H_CAPACITY <= IPC_RING_REGION_END,
    "target->host ring overflows the m33_m55_shared region");
#endif

/* Called once by the producer before any transfer.
 * `capacity` selects the direction's size (IPC_RING_H2T_CAPACITY / IPC_RING_T2H_CAPACITY). */
static inline void ipc_ring_init(ipc_ring_t *r, uint32_t capacity) {
    r->head = 0u;
    r->tail = 0u;
    r->capacity = capacity;
    __DMB();
    r->magic = IPC_RING_MAGIC;
    __DMB();
}

static inline uint32_t ipc_ring_count(const ipc_ring_t *r) {
    return r->head - r->tail;               /* correct under 32-bit wrap */
}

static inline uint32_t ipc_ring_free(const ipc_ring_t *r) {
    return r->capacity - (r->head - r->tail);
}

/* Producer: append up to `len` bytes; returns the number actually written. */
static inline size_t ipc_ring_write(ipc_ring_t *r, const uint8_t *src, size_t len) {
    if (r->magic != IPC_RING_MAGIC) {
        return 0u;                           /* ring not initialized yet */
    }
    uint32_t cap = r->capacity;
    uint32_t space = cap - (r->head - r->tail);
    if (len > space) {
        len = space;
    }
    if (len == 0u) {
        return 0u;
    }
    uint32_t idx = r->head & (cap - 1u);
    size_t first = (size_t)(cap - idx);
    if (first > len) {
        first = len;
    }
    memcpy(&r->data[idx], src, first);
    if (len > first) {
        memcpy(&r->data[0], src + first, len - first);
    }
    __DMB();                                 /* data visible before head moves */
    r->head += (uint32_t)len;
    __DMB();
    return len;
}

/* Consumer: read up to `len` bytes; returns the number actually read. */
static inline size_t ipc_ring_read(ipc_ring_t *r, uint8_t *dst, size_t len) {
    if (r->magic != IPC_RING_MAGIC) {
        return 0u;                           /* ring not initialized yet */
    }
    uint32_t avail = r->head - r->tail;
    __DMB();                                 /* order head load before data reads */
    if (len > avail) {
        len = avail;
    }
    if (len == 0u) {
        return 0u;
    }
    uint32_t idx = r->tail & (r->capacity - 1u);
    size_t first = (size_t)(r->capacity - idx);
    if (first > len) {
        first = len;
    }
    memcpy(dst, &r->data[idx], first);
    if (len > first) {
        memcpy(dst + first, &r->data[0], len - first);
    }
    __DMB();                                 /* data copied before tail moves */
    r->tail += (uint32_t)len;
    __DMB();
    return len;
}

#endif /* IPC_RING_H */
