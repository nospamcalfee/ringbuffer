/*
 * Copyright 2024, Hiroyuki OYAMA. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "ring_buffer.h"
#include <math.h>
#include "crc.h"
#include <string.h>

//ring buffer code

crc_t crc_update(crc_t crc, const void *data, size_t data_len)
{
    const unsigned char *d = (const unsigned char *)data;
    unsigned int i;
    crc_t bit;
    unsigned char c;

    while (data_len--) {
        c = *d++;
        for (i = 0x01; i & 0xff; i <<= 1) {
            bit = (crc & 0x10) ^ ((c & i) ? 0x10 : 0);
            crc <<= 1;
            if (bit) {
                crc ^= 0x05;
            }
        }
        crc &= 0x1f;
    }
    return crc & 0x1f;
}

static rb_errors_t is_crc_good(rb_header *rbh) {
    crc_t crc = crc_init();
    crc = crc_update(crc, rbh, 3);
    crc = crc_finalize(crc);
    //remove any used flags from crc check
    if ((rbh->crc & ~RB_HEADER_SPLIT) != crc) return RB_BAD_HDR;
    return RB_OK; //for now no crc check
}
static rb_errors_t is_header_good(rb_header *rbh) {
    if (rbh->id == 0xff && rbh->crc == 0xff && rbh->len == RB_MAX_LEN_VALUE) {
        return RB_BLANK_HDR;
    }
    if (rbh->id == 0xff || rbh->len == 0 || rbh->len > MAX_SECTS * FLASH_SECTOR_SIZE) {
        return RB_BAD_HDR;
    }
    return is_crc_good(rbh);
}

static rb_errors_t make_header(rb_header *rbh, uint8_t id, uint32_t size) {
    if (rbh == NULL || size == 0 || id == 0xff) {
        return RB_BAD_CALLER_DATA;
    }
    rbh->id = id;
    rbh->len = size;
    rbh->crc = crc_init();
    rbh->crc = crc_update(rbh->crc, rbh, 3);
    rbh->crc = crc_finalize(rbh->crc);
    return RB_OK;
}
/*
 Increment offset to next ring address. If it steps out of a sector, wrap to
 next sector or around to first sector if necessary. If it steps within a
 sector but there isnt room for at least a header and one data byte, skip to
 next sector. irregardless of len argument the max it will increment is to the
 next sector.
*/
static uint32_t rb_incr(uint32_t oldlen, uint32_t len, uint32_t maxlen){
    uint32_t nextaddr;
    if (len > FLASH_SECTOR_SIZE) {
        //take a really big step, skipping to next sector header
        nextaddr = FLASH_SECTOR(oldlen) + FLASH_SECTOR_SIZE;
    } else if (MOD_SECTOR(oldlen) + len > (FLASH_SECTOR_SIZE - (sizeof(rb_header) + 1) )) {
        //doesnt fit in this sector go to next sector
        nextaddr = FLASH_SECTOR(oldlen) + FLASH_SECTOR_SIZE;
    } else {
        //fits in this sector, get new offset
        nextaddr = oldlen + len;
    }
    if (nextaddr >= maxlen) {
        // wrap back to start of ring
        nextaddr = 0;
    }

    return nextaddr;
}
static rb_errors_t fetch_and_check_header(rb_t *rb, rb_header *phdr, int jumpto) {
    uint32_t nextoffs = (rb->next + jumpto);
    assert(nextoffs < __PERSISTENT_LEN);
    flash_read(rb->base_address + nextoffs, phdr, sizeof(*phdr));
    return is_header_good(phdr);
}

static int count_blanks(uint8_t *buffer, uint8_t value, int maxscan) {
    for (int i = 0; i < maxscan; i++){
        if (buffer[i] != value) return i; //count of matches
    }
    return maxscan; //all blank
}
static int sector_blank_scan(rb_t *rb) {
    //count blanks remaining in sector
    uint32_t size_in_sector;
    size_in_sector = FLASH_SECTOR_SIZE - MOD_SECTOR(rb->next);
    uint32_t blanks = count_blanks((uint8_t *)XIP_NOCACHE_NOALLOC_BASE + rb->base_address + rb->next, 0xff, size_in_sector);
    if (blanks == size_in_sector) {
        //rest of this sector is blank, check next sector
        uint32_t offs = FLASH_SECTOR(rb->next) + FLASH_SECTOR_SIZE;
        if (offs > __PERSISTENT_LEN) {
            offs = 0; //wrap around flash allocation
        }
        uint32_t nextblanks = count_blanks((uint8_t *)XIP_NOCACHE_NOALLOC_BASE + rb->base_address + offs, 0xff, FLASH_SECTOR_SIZE);
        blanks += nextblanks;
    } 
    return blanks;
}
/* 

find next writeable flash offset. start at first flash sector, looking for valid
data, update rb ring ptrs. Return error code, side effect, update ring
pointers. Writes will always leave at least room for one more hdr and will fail
if there is not enough room.

*/
static rb_errors_t rb_findnext_writeable(rb_t *rb) {
    rb_header hdr;
    uint32_t origrb = FLASH_SECTOR(rb->next); //start of this page
    assert(rb->next < rb->number_of_bytes);
    rb_errors_t hdr_res = RB_BAD_HDR;
    if (rb == NULL ) {
        return RB_BAD_CALLER_DATA; // Error handling: Null pointer passed
    }
    do {
        if (MOD_SECTOR(rb->next) > FLASH_SECTOR_SIZE - sizeof(hdr) - 1) {
            //skip last few bytes of sector
            rb->next +=  FLASH_SECTOR_SIZE - MOD_SECTOR(rb->next);
            if (rb->next >= __PERSISTENT_LEN) {
                rb->next = 0; //wrap to first sector allocated
            }
        }
        hdr_res = fetch_and_check_header(rb, &hdr, 0); //fetch and check header
        switch (hdr_res) {
        case RB_OK: //legit hdr, update ptrs
            /* 
            if header is at end of wrapped rb return status. Writer will split
            user data over sectors, but never a header. Writer will write a
            header at the start of every sector. Reader will detect that last
            block in sector does not include a space for at least 1 user byte
            and will skip to next sector
            */
            break;
        case RB_BLANK_HDR:
            //blank headers are ok, found next available in rb, all done
            return hdr_res;
        default:
            return hdr_res; //return errors here
         }
        //found a good header, use it to skip ahead
        //keep looking for end of rb
        uint32_t oldnext = rb->next;
        rb->next = rb_incr(rb->next, hdr.len + sizeof(hdr), rb->number_of_bytes);
        if (rb->next == origrb){
            //data in flash is full, we wrapped.
            return RB_HDR_LOOP;
        }
        hdr_res = fetch_and_check_header(rb, &hdr, 0);
        if (hdr_res != RB_OK && hdr_res != RB_BLANK_HDR) {
            return hdr_res;
        }
        if (FLASH_SECTOR(rb->next) != FLASH_SECTOR(oldnext)) {
            // if we wrap to next sector, fixup to match required sector header
            hdr_res = fetch_and_check_header(rb, &hdr, 0); //fetch and check header
        }
    } while (1);
    return hdr_res;
}
/*
 The ring will start in the first sector with a good header. If all sectors are
 blank, then it will start in the first sector.
*/
static rb_errors_t rb_find_ring_start(rb_t *rb) {
    rb_header hdr;
    rb_errors_t hdr_res = RB_BAD_HDR;
    if (rb == NULL ) {
        return RB_BAD_CALLER_DATA; // Error handling: Null pointer passed
    }
    rb->next = 0; //start on first sector
    do {
        flash_read(rb->base_address + rb->next, &hdr, sizeof(hdr));
        hdr_res = is_header_good(&hdr);
        switch (hdr_res) {
        case RB_OK: //legit hdr, update ptrs, start here
            return hdr_res;
        case RB_BLANK_HDR: //header is erased, keep looking?
            break;
        default:
            return hdr_res; //some error finding start
        }
        rb->next += FLASH_SECTOR_SIZE;
    } while (rb->next < rb->number_of_bytes);
    //we have searched the ring, no hdr found, so start at ring start
    rb->next = 0;
    return hdr_res;
}
// here I know entire write will be in this sector, maybe multiple pages
static rb_errors_t rb_append_page(rb_t *rb, rb_header * hdr, const void *data, uint32_t size) {
    uint32_t pagerem;   //offset in temp ram buffer
    uint32_t wrlen;     //amount written so far
    uint32_t inlen = 0; //offset into data buffer
    int hdrsize = sizeof(*hdr);

    if (MOD_PAGE(rb->next)) {
        //need to load previously written data page aligned
        flash_read(rb->base_address + FLASH_PAGE(rb->next), rb->rb_page, sizeof(rb->rb_page));
    } else {
        //new page, no need to read current stuff
        memset(&rb->rb_page, 0xff, sizeof(rb->rb_page)); 
    }
    memcpy(&rb->rb_page[MOD_PAGE(rb->next)], hdr, hdrsize);
    rb->next += hdrsize;

    //fixme if writing 16 bytes at offset 16 pgsize-hdr-existingamount
    pagerem = FLASH_PAGE_SIZE - MOD_PAGE(rb->next);
    // amount left on page, should be > hdrsize
    assert(pagerem > 0);

    wrlen = MIN(pagerem, size); //first page data size
    if (wrlen > 0) {
        //fixme here and below, when copying less than a full flash page of data, never leave a gap at the end of less than hdr size!
        //copy in some actual data bytes
        memcpy(&rb->rb_page[MOD_PAGE(rb->next)], data, wrlen);
        //write buffered page into flash, first page.
        flash_prog(rb->base_address + FLASH_PAGE(rb->next), &rb->rb_page, sizeof(rb->rb_page));
        rb->next += wrlen;
        inlen += wrlen;
        wrlen = size - wrlen; //remaining amount to write
    }
    while (wrlen > 0) {
        if (wrlen >= FLASH_PAGE_SIZE) {
            //write a full page
            flash_prog(rb->base_address + FLASH_PAGE(rb->next), data + inlen, FLASH_PAGE_SIZE);
            rb->next += FLASH_PAGE_SIZE;
            inlen += FLASH_PAGE_SIZE;
            wrlen = wrlen - FLASH_PAGE_SIZE; //remaining amount to write
        } else {
            //finally at the last page to write.
            //This data is less than a page, so blank fill it first
            memset(&rb->rb_page, 0xff, sizeof(rb->rb_page));
            memcpy(&rb->rb_page, data + inlen, wrlen );
            flash_prog(rb->base_address + FLASH_PAGE(rb->next), &rb->rb_page, FLASH_PAGE_SIZE);
            rb->next += wrlen;
            inlen += wrlen;
            wrlen = 0; //remaining amount to write
        }
        //write all the rest of the data into flash
    } //end while
    if (rb->next >= rb->number_of_bytes) {
        rb->next = 0; //wrap to first sector
    }
    return RB_OK;
}
/*
  We have weird sector and page boundaries to deal with. If a write will fit in
  a sector, go ahead and write the pages. If it won't fit in a sector split the
  write at the sector boundary. For the first part, it will now fit, do a page
  write. For the last part over the current sector, Create a new header and
  write the next part. It is possible the next part will itself split over a
  sector boundary, but repeat as necessary.
*/
static rb_errors_t rb_sector_append(rb_t *rb, rb_header * hdr, const void *data, uint32_t size) {
    rb_errors_t hdr_res;
    int id = hdr->id;
    int hdrsize = sizeof(*hdr);
    uint32_t size_needed_in_sector;
    if (rb == NULL || data == NULL || size == 0 || hdr == NULL || hdr->id >= 0xff) {
        return RB_BAD_CALLER_DATA;
    }
    size_needed_in_sector = size + hdrsize;
    uint32_t blank_cnt = sector_blank_scan(rb);
    if (blank_cnt < size_needed_in_sector) return RB_FULL;
    if (size_needed_in_sector < FLASH_SECTOR_SIZE) {
        //write will fit this flash sector, write pages
        hdr_res = rb_append_page(rb, hdr, data, size);
    } else {
        //write will span two sectors, I assume current sector is good
        rb_header rbh2;
        uint32_t size_in_sector = FLASH_SECTOR_SIZE - MOD_SECTOR(rb->next) - hdrsize;
        uint32_t nextsector =  FLASH_SECTOR(rb->next) + FLASH_SECTOR_SIZE;
        if (nextsector >= __PERSISTENT_LEN) {
            nextsector = 0; //wrap to first sector allocated
        }
        //check header for next flash sector at start of sector
        hdr_res = fetch_and_check_header(rb, &rbh2, nextsector - rb->next);
        if (hdr_res != RB_BLANK_HDR) {
            if(hdr_res == RB_OK) return RB_WRAPPED_SECTOR_USED;
            return hdr_res;
        }

        hdr_res = fetch_and_check_header(rb, &rbh2, size_in_sector);
        if (hdr_res == RB_BLANK_HDR) {
            //Only continue if next sector is blank, otherwise caller handles it.

            hdr_res = make_header(&rbh2, id, size_in_sector);
            hdr_res = rb_append_page(rb, &rbh2, data, size_in_sector);
            if (hdr_res != RB_OK) return hdr_res;
            //this will recurse and keep writing pages
            assert( MOD_SECTOR(rb->next) == 0);
            //if we are in the next sector, is it blank?
            hdr_res = make_header(&rbh2, id, size - size_in_sector);
            rbh2.crc |= RB_HEADER_SPLIT; //set flag in flash header
            hdr_res = rb_sector_append(rb, &rbh2, data, size - size_needed_in_sector);
        }
    }
    return hdr_res;
}
// every call will flash the involved sector(s), even tiny data
rb_errors_t rb_append(rb_t *rb, uint8_t id, const void *data, uint32_t size) {
    rb_errors_t hdr_res;
    rb_header rbh;
    if (rb == NULL || data == NULL || size == 0 || id == 0xff || 
        size > (__PERSISTENT_LEN - sizeof(rbh))) {
        return RB_BAD_CALLER_DATA;
    }

    //rbcreate and other appends guarantee pointers are good in rb
    hdr_res = rb_findnext_writeable(rb); //get pointers in rb
    if (hdr_res != RB_BLANK_HDR) return hdr_res;
    hdr_res = make_header(&rbh, id, size);
    hdr_res = rb_sector_append(rb, &rbh, data, size); //fixme handle errors! 
//     append_flash_memory(cb, data, size);
//     erase_next_flash_sector_if_necessary(cb);
//     update_buffer_append_state(cb);
    return hdr_res;
}
/*
    read up to size data bytes into data buffer, of next flash which matches id
*/
int rb_read(rb_t *rb, uint8_t id, void *data, uint32_t size) {
    rb_errors_t hdr_res;
    rb_header hdr;
    uint32_t orignext = FLASH_SECTOR(rb->next); //save start of search
    if (rb == NULL || data == NULL || size == 0 || id == 0xff || 
        size > (__PERSISTENT_LEN - sizeof(hdr))) {
        return -RB_BAD_CALLER_DATA;
    }
    do {
        hdr_res = fetch_and_check_header(rb, &hdr, 0); //fetch and check header
        switch (hdr_res) {
        case RB_OK: //legit hdr, read this
            rb->next += sizeof(hdr);
            break;
        default:
            return -hdr_res; //return errors here
        }
        if (hdr.id != id) {
            //not my data, keep looking
            rb->next = rb_incr(rb->next, hdr.len + sizeof(hdr), rb->number_of_bytes);
            if (orignext == rb->next) return -RB_HDR_ID_NOT_FOUND;
            continue; //do loop again
        }
        //found a good header, use it to read data, maybe split into two reads
        uint32_t size_in_sector = FLASH_SECTOR_SIZE - MOD_SECTOR(rb->next);
        uint32_t remaining_size = size;
        uint32_t read_size = MIN(size_in_sector, size);
        flash_read(rb->base_address + rb->next, data, read_size);
        rb->next += read_size;
        remaining_size -= read_size;
        data += read_size;
        while (remaining_size) {
            read_size = MIN(FLASH_SECTOR_SIZE, remaining_size);
            flash_read(rb->base_address + rb->next, data, read_size);
            rb->next += read_size; //fixme next needs to wrap around buffer
            remaining_size -= read_size;
            data += read_size;
        }
        break;
    } while (1);
    if (hdr_res == RB_OK) {
        return size; //fixme - if returned data is too long or short return actual size
    } else {
        return -hdr_res;
    }
}
/* 
Create a new variable sized ringbuffer control block, maybe erasing the whole
thing. Also, for writes find the next writeable area. 
*/
rb_errors_t rb_create(rb_t *rb, uint32_t base_address, 
                      size_t number_of_sectors, bool force_initialize,
                      bool write_buffer) {
    rb_errors_t hdr_err = RB_BAD_HDR;
#ifdef PICO_FLASH_SIZE_BYTES
    if (number_of_sectors > PICO_FLASH_SIZE_BYTES / FLASH_SECTOR_SIZE) {
        return RB_BAD_CALLER_DATA;
    }
#endif
    if (rb == NULL || number_of_sectors < 1) {
        return RB_BAD_CALLER_DATA;
    }
     //offset in flash, not system address
    rb->base_address = base_address % XIP_BASE;
    rb->number_of_bytes = number_of_sectors * FLASH_SECTOR_SIZE;
    rb->next = 0;

    if (force_initialize) {
        flash_erase(rb->base_address, rb->number_of_bytes);
        hdr_err = RB_OK;
    } else {
        //request was to continue in rb as exists in flash.
        hdr_err = rb_find_ring_start(rb);
        if (hdr_err == RB_OK || hdr_err == RB_BLANK_HDR) {
            if (write_buffer) {
                hdr_err = rb_findnext_writeable(rb);
            }
        }
    }
    //it is up to the user to deal with rb errors
    return hdr_err;
}
