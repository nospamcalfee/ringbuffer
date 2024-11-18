/*
 * Copyright 2024, Steve Calfee. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 * Idea spawned from circular_buffer app
 * Copyright 2024, Hiroyuki OYAMA. All rights reserved.
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
 re-engineered to allow variable length flash storage.
 The linker should declare flash sectors and locations, but
 the user can have multiple flash rings set up with cb_create

 Sector rings are really the ring buffer, internal data also implemented here
 fits inside sector rings. Rings can be 1 to n sectors. If 1 sector, there is a
 vulnerability to lose data during erase and re-write, on a restart. With
 multiple sector rings the problem is on restart to detect the newest and
 oldest sectors. So every sector has an additional 4 byte header with a 24 bit
 index and a 5 bit crc for the sector ring header. The ring index goes
 monotonically up to (number_of_bytes / FLASH_SECTOR_SIZE), so the index can be
 used to determine where the ring has wrapped, even if all previous pages are
 full. Assuming flashing is rare, after all the flash wears out at about
 100,000 erases, I don't think we need to worry about wrapping the flash
 index - the flash will be dead by then anyway.

 Rings will be checked when used, if invalid, status is returned so caller can
 erase and start over. Writes are especially risky and should be rare. If
 multiple writers are used, the ring must be checked before every write. ring
 buffer headers cache local accessor data, but not global, so must be rebuilt
 before every write. Write speed will be sacrificed to assure data integrity.

 Erased sectors are all 0xff bytes. Sectors start at the lowest addressed
 sector, and rb_header(s) can be followed to find the last sector used on any
 system startup

 Data rings start at the lowest sector allocated with a rb_sector_header and
 rb_header, user data etc. Users can write from 1 byte to 64K-4 bytes long
 limited by the len field in rb_header. If less than 64K is allocated in flash
 sector space, only that much less 8 (rb_sector_header plus one rb_header) is
 the maximum write size. User data can span over a sector, each new sector
 entered will be checked for blank and erased (and header added) if requested
 on the write. Or the write will fail before starting. The new sector will have
 a rb_sector_header and a rb_header with an adjusted len at the start. This
 preserves the ring when old data is erased, but old data may lose its head if
 it spans a sector, and older data is overwritten.

 Oldest data sectors are found using the sector index number - lowest index is
 oldest. When the oldest is found, the internal data is skipped, until a new
 blank data area is found or the data area is full.

 Reads are kind of tricky. It is assumed that the user will (for any id) read
 the same amount as was written. A request that asks for more data than was
 read will return the actual amount read. It is generally intended that the ring
 buffering handling be invisible from the caller. He writes a bunch of bytes
 for an id and reads it back later. Flash pages, sectors and header gaps are
 not the applications problem.

 If the read request is too short, the buffer pointers should skip to the end of
 the id data. If the read request is too long (like for some utility program
 that did not actually write the data), the entire flash data will be read and
 the actual length of the read returned. This means the read should return
 negative numbers for errors and the actual length read on a successful read
 return.

 The only write possible is an append to the end of the ring buffer. So appends
 always will find the oldest data and add after that (if there is room).

 Reads keep track of where the last read was completed, so multiple reads will
 read subsequent records with a matching id. If the user wants to rewind and
 restart reading a new rb_recreate used to set up the buffer pointers. The user
 should not access the rb pointers.

 rb_delete finds the next matching id (and if requested matching data). Then it
 simply erases one bit in the record header marking the record as deleted.

 One user ring buffer can be used for both reads and writes to the same flash
 area, because the internal data is maintained separately. That is, the
 internal write routines can do reads, but the external read pointer is
 maintained.

*/

// Declaration of functions related to the ring buffer
typedef struct {
    uint16_t len;  //blob size - used to find next storage item.
    uint8_t id;    //multiple kinds of user data can be saved, must not be 0xff OR 0
    uint8_t crc;   //validity check of rb_header
} rb_header;

/*
    sectors also have a structure and 1 word overhead per sector. ASSUMPTION,
    both rb_header and rb_sector_header are the same size.
*/
typedef struct {
    uint32_t header;    //27 bits of index, 5 bits for crc, use accessors
} rb_sector_header;

// #define RB_INDEX_MASK 0x7ffffff 5 bit crc
#define RB_INDEX_MASK 0xffffff
// static inline uint32_t get_crc (rb_sector_header *p) {return p->header & 0x1f;}
// static inline void set_index (rb_sector_header *p, uint32_t n) { p->header = (n & RB_INDEX_MASK) << 5 | get_crc(p);}
// static inline int get_index (rb_sector_header *p) {return p->header >> 5;}
// static inline void set_crc (rb_sector_header *p, uint32_t n) { p->header = get_index(p) << 5 | (n & 0x1f);}
static inline uint32_t get_crc (rb_sector_header *p) {return p->header & 0xff;}
static inline void set_index (rb_sector_header *p, uint32_t n) { p->header = (n & RB_INDEX_MASK) << 8 | get_crc(p);}
static inline uint32_t get_index (rb_sector_header *p) {return p->header >> 8;}
static inline void set_crc (rb_sector_header *p, uint32_t n) { p->header = get_index(p) << 8 | (n & 0xff);}

#define HEADER_SIZE (sizeof(rb_header))
//get highest legal value for len in rb_header
#define RB_MAX_LEN_VALUE ((uint16_t) -1)
//highest legal write size
#define RB_MAX_APPEND_SIZE (FLASH_SECTOR_SIZE - sizeof(rb_sector_header) - sizeof(rb_header))
//given a binary power, return the modulo2 mask of its value
#define MOD_MASK(a) (a - 1)
//if the flash erase sector is not binary, replace below with a % operation
#define MOD_SECTOR(a) ((a) & MOD_MASK(FLASH_SECTOR_SIZE))
#define MOD_PAGE(a) ((a) & MOD_MASK(FLASH_PAGE_SIZE))
//define amount to round an address to a uint32 boundary
#define ROUND_UP(a) ((sizeof(uint32_t)) - ((a) & MOD_MASK(sizeof(uint32_t))) & (MOD_MASK(sizeof(uint32_t))))
// change arg to page or sector without offset in page or sector
#define FLASH_PAGE(a) ((a) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)
#define FLASH_SECTOR(a) ((a) / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE)
//the crc is only 5 bits, use upper 3 bits as flags written to flash
#define RB_HEADER_SPLIT (1<<7)
//and there are 2 other non-crc bits that can be used
//they are created as 1 bits and can be erased anytime to zero as needed
#define RB_HEADER_NOT_SMUDGED (1<<6)
#define RB_HEADER_UNUSED (1<<5)
#define ARRAY_LENGTH(array) (sizeof (array) / sizeof (const char *))

/* Variable size ring buffer, need one struct per accessor to/from flash. next
   entry could be used to determine amount used, except for the ring wrapping,
   which is data dependent.
*/
typedef struct {
    uint32_t base_address; //offset in flash, not system address
    uint32_t number_of_bytes;
    uint32_t next; //working read pointer into flash ring 0<=next<number_of_bytes
    uint32_t last_wrote; //info for caller as to where in rb last written
    uint32_t sector_index; //track for ring wraps.
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

//define initialize choices
enum init_choices {
    CREATE_FAIL,
    CREATE_INIT_IF_FAIL,
    CREATE_INIT_ALWAYS,
};

rb_errors_t rb_create(rb_t *rb, uint32_t base_address,
                      size_t number_of_sectors, enum init_choices init_choice) ;
//helper to create and re-create (if data is bad) a buffer control block
rb_errors_t rb_recreate(rb_t *rb, uint32_t base_address,
                            size_t number_of_sectors, enum init_choices init_choice);
//page buffer must be passed with a full page of temp buffer for writes
rb_errors_t rb_append(rb_t *rb, uint8_t id, const void *data, uint32_t size,
                      uint8_t *pagebuffer, bool erase_if_full);
int rb_read(rb_t *rb, uint8_t id, void *data, uint32_t size);
int rb_find(rb_t *rb, uint8_t id, const void *data, uint32_t size, uint8_t *scratch);
/* given a writeable page, delete a matching id, string entry */
rb_errors_t rb_delete(rb_t *rb, uint8_t id, const void *data, uint32_t size, uint8_t *pagebuffer);
rb_errors_t rb_check_sector_ring(rb_t *rb);
//get defines from the .ld link map
//users can divide this flash space as they wish
extern char __flash_persistent_start;
extern char __flash_persistent_length;
#define __PERSISTENT_TABLE  ((uint32_t) &__flash_persistent_start)
#define __PERSISTENT_LEN    ((uint32_t) &__flash_persistent_length)

#endif
