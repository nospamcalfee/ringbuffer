/*
 * Copyright 2024, Hiroyuki OYAMA. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _RING_BUFFER_H_
#define _RING_BUFFER_H_

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
 Users can write from 1 byte to 64K-4 bytes long. If less than 64K is allocated
 in flash sector space, only that much less 4 is the maximum write size.
 Userdata can span over a sector, each new sector entered will be checked for
 blank and erased if requested. Or the save will fail before starting. The new
 sector will have a rb_header with an adjusted len at the start. This preserves
 the ring when old data is erased, but old data may lose its head if it spans a
 sector.

 Newest data is in the first sector with incomplete usage by tracing headers
 and lens in rb_header.

 Reads are kind of tricky. It is assumed that the user will (for any id) read the
 same amount as was written. It is generally intended that the ring buffering
 handling be invisible from the caller. He writes a bunch of bytes for an id
 and reads it back later. Flash pages, sectors and header gaps are not the
 applications problem.

 FIXME - as an improvement any size reads should be attempted. If the read
 request is too short, the buffer pointers should skip to the end of the id
 data. If the read request is too long (like for some utility program that did
 not actually write the data), the entire data storage should be read and the
 actual length of the read returned. This means the read should return negative
 numbers for errors and the actual length read on a successful read return.

*/

// Declaration of functions related to the ring buffer
typedef struct {
    uint16_t len;  //blob size - used to find next storage item.
    uint8_t id;    //multiple kinds of user data can be saved, must not be 0xff
    uint8_t crc;   //validity check of rb_header
} rb_header;

#define HEADER_SIZE (sizeof(rb_header))
//get highest legal value for len in rb_header
#define RB_MAX_LEN_VALUE ((uint16_t) -1)
#define MAX_SECTS (__PERSISTENT_LEN / FLASH_SECTOR_SIZE)

//given a binary power, return the modulo2 mask of its value
#define MOD_MASK(a) (a - 1)
#define MOD_SECTOR(a) ((a) & MOD_MASK(FLASH_SECTOR_SIZE))
#define MOD_PAGE(a) ((a) & MOD_MASK(FLASH_PAGE_SIZE))
//define amount to round an address to a uint32 boundary
#define ROUND_UP(a) ((sizeof(uint32_t)) - ((a) & MOD_MASK(sizeof(uint32_t))) & (MOD_MASK(sizeof(uint32_t))))
// change arg to page or sector without offset in page or sector
#define FLASH_PAGE(a) ((a) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)
#define FLASH_SECTOR(a) ((a) / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE)
//the crc is only 5 bits, use upper 3 bits as flags written to flash
#define RB_HEADER_SPLIT (1<<7)

/* Variable size ring buffer, need one per accessor to/from flash */
typedef struct {
    uint32_t base_address; //offset in flash, not system address
    uint32_t number_of_bytes;
    uint32_t next; //working pointer into flash ring 0<=next<FLASH_SECTOR_SIZE
    uint8_t *rb_page; //only required for writes.
} rb_t;

typedef enum {
    RB_OK = 0,
    RB_BAD_CALLER_DATA,
    RB_BAD_SECTOR,
    RB_BLANK_HDR,
    RB_BAD_HDR,
    RB_WRAPPED_SECTOR_USED = 5,
    RB_HDR_LOOP,
    RB_HDR_ID_NOT_FOUND,
    RB_FULL,
} rb_errors_t;

rb_errors_t rb_create(rb_t *rb, uint32_t base_address, 
                      size_t number_of_sectors, bool force_initialize,
                      bool write_buffer);
//page buffer must be passed with a full page of temp buffer for writes
rb_errors_t rb_append(rb_t *rb, uint8_t id, const void *data, uint32_t size,
                      uint8_t *pagebuffer);
int rb_read(rb_t *rb, uint8_t id, void *data, uint32_t size);
//get defines from the .ld link map
extern char __flash_persistent_start;
extern char __flash_persistent_length;
#define __PERSISTENT_TABLE  ((uint32_t) &__flash_persistent_start)
#define __PERSISTENT_LEN    ((uint32_t) &__flash_persistent_length)

#endif
