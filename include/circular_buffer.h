/*
 * Copyright 2024, Hiroyuki OYAMA. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _CIRCULAR_BUFFER_H_
#define _CIRCULAR_BUFFER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hardware/flash.h>
#include <pico/stdlib.h>

#include "flash.h"


typedef uint64_t (*timestamp_extractor_t)(void *entry);

/* 
 reengineered to allow variable length flash storage.
 The linker should declare flash sectors and locations, but
 the user can have multiple flash rings set up with cb_create

 Rings can be 1 to n sectors. If 1 sector, there is a vulnerability to lose
 data during erase and re-write, on a restart.

 Rings will be checked when used, if invalid, status is returned
 so caller can erase and start over.

 Erased sectors are all 0xff bytes. Sectors start at the lowest addressed
 sector, and rb_header can be followed to find the last sector used on any
 system startup

 Rings start at the lowest sector allocated with a rb_header, userdata etc.
 Userdata can span over a sector, each new sector entered will be checked for
 blank and erased if requested. Or the save will fail before starting. The
 new sector will have a rb_header with an adjusted len at the start. This
 preserves the ring when old data is erased, but old data may lose its head.

 Newest data is in the first sector with incomplete usage by tracing headers
 and lens in rb_header.

*/

typedef struct {
    uint8_t id;    //multiple kinds of user data can be saved, must not be 0xff
    uint8_t crc;   //validity check of rb_header
    uint16_t len;  //blob size - used to find next storage item.
} rb_header;

#define RB_MAX_LEN_VALUE 0xffff

typedef enum {
    RB_CURSOR_DESCENDING,
    RB_CURSOR_ASCENDING
} cb_cursor_order_t;


// Structure to manage the ring buffer
typedef struct {
    uint64_t address;
    size_t storage_size;
    size_t length;
    size_t item_size;
    size_t head;
    size_t tail;
    size_t items_per_page;
    size_t pages_per_sector;
    size_t total_sectors;
    bool is_full;
    timestamp_extractor_t get_timestamp;
} cb_t;

// Structure to manage a cursor for ring buffer operations
typedef struct {
    cb_t *cb;
    size_t index;
    cb_cursor_order_t order;
} cb_cursor_t;

// Declaration of functions related to the ring buffer
int cb_create(cb_t *cb, uint32_t address, size_t length, size_t item_size,
              timestamp_extractor_t get_timestamp, bool force_initialize);
void cb_append(cb_t *cb, const void *data, size_t size);
void cb_open_cursor(cb_t *cb, cb_cursor_t *cursor, cb_cursor_order_t order);
bool cb_get_next(cb_cursor_t *cursor, void *data);
void cb_close_cursor(cb_cursor_t *cursor);

/* new variable size ring buffer */
typedef struct {
    size_t base_address; //offset in flash, not system address
    size_t number_of_bytes;
    size_t next; //working pointer into flash ring
    bool is_full;
} rb_t;

typedef enum {
    RB_OK = 0,
    RB_BAD_CALLER_DATA,
    RB_BAD_SECTOR,
    RB_BLANK_HDR,
    RB_BAD_HDR,
} rb_errors_t;

rb_errors_t rb_create(rb_t *rb, uint32_t address, size_t sectors, bool force_initialize);

//get defines from the .ld link map
extern char __flash_persistent_start;
extern char __flash_persistent_length;
#define __PERSISTENT_TABLE  ((uint32_t) &__flash_persistent_start)
#define __PERSISTENT_LEN    ((uint32_t) &__flash_persistent_length)

#endif
