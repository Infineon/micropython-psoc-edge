/*
 * ipc_ring.h — Lock-free SPSC byte-stream ring buffers in shared SRAM.
 *
 * Two rings, one per direction, each living in a 4 KB shared page that the
 * producing core owns:
 *   host   -> target  (CM33 -> CM55)  in m33_allocatable_shared (0x240FE000)
 *   target -> host    (CM55 -> CM33)  in m55_allocatable_shared (0x240FF000)
 *
 * Phase 1: the whole 0x240FD000-0x240FFFFF window is configured Normal
 * Non-Cacheable by the CM55 MPU, so no cache maintenance is required — only
 * __DMB() ordering barriers between the data copy and the head/tail update.
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

/* Shared, non-cacheable SRAM pages (see cycfg_system.c MPU config). */
#define IPC_RING_HOST_TO_TARGET_ADDR   (0x240FE000u)  /* m33_allocatable_shared */
#define IPC_RING_TARGET_TO_HOST_ADDR   (0x240FF000u)  /* m55_allocatable_shared */

#define IPC_RING_MAGIC       (0x52494E47u)  /* 'RING' */
#define IPC_RING_CAPACITY    (2048u)        /* power of two; usable bytes/dir  */

/*
 * head/tail are free-running byte counters (never masked in storage); the
 * physical index is (counter & (IPC_RING_CAPACITY - 1)). This makes the
 * full/empty distinction unambiguous and uses the whole data region.
 */
typedef struct {
    uint32_t magic;                             /* IPC_RING_MAGIC once inited  */
    uint32_t capacity;                          /* == IPC_RING_CAPACITY        */
    volatile uint32_t head;                     /* producer: total bytes in    */
    volatile uint32_t tail;                     /* consumer: total bytes out   */
    uint8_t data[IPC_RING_CAPACITY];
} ipc_ring_t;

#define IPC_RING_HOST_TO_TARGET  ((ipc_ring_t *)IPC_RING_HOST_TO_TARGET_ADDR)
#define IPC_RING_TARGET_TO_HOST  ((ipc_ring_t *)IPC_RING_TARGET_TO_HOST_ADDR)

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(ipc_ring_t) <= 0x1000, "ipc_ring_t must fit in a 4 KB shared page");
#endif

/* Called once by the producer (page owner) before any transfer. */
static inline void ipc_ring_init(ipc_ring_t *r) {
    r->head = 0u;
    r->tail = 0u;
    r->capacity = IPC_RING_CAPACITY;
    __DMB();
    r->magic = IPC_RING_MAGIC;
    __DMB();
}

static inline uint32_t ipc_ring_count(const ipc_ring_t *r) {
    return r->head - r->tail;               /* correct under 32-bit wrap */
}

static inline uint32_t ipc_ring_free(const ipc_ring_t *r) {
    return IPC_RING_CAPACITY - (r->head - r->tail);
}

/* Producer: append up to `len` bytes; returns the number actually written. */
static inline size_t ipc_ring_write(ipc_ring_t *r, const uint8_t *src, size_t len) {
    if (r->magic != IPC_RING_MAGIC) {
        return 0u;                           /* ring not initialized yet */
    }
    uint32_t space = IPC_RING_CAPACITY - (r->head - r->tail);
    if (len > space) {
        len = space;
    }
    if (len == 0u) {
        return 0u;
    }
    uint32_t idx = r->head & (IPC_RING_CAPACITY - 1u);
    size_t first = (size_t)(IPC_RING_CAPACITY - idx);
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
    uint32_t idx = r->tail & (IPC_RING_CAPACITY - 1u);
    size_t first = (size_t)(IPC_RING_CAPACITY - idx);
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
