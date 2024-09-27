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
    crc = crc_update(crc, rbh, 3); //fixme assumes little endian?
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

static rb_errors_t is_sector_header_good(rb_sector_header *shdr) {
    if ((int)shdr->header == -1) {
        return RB_BLANK_HDR;
    }
    uint32_t data = get_index(shdr);
    crc_t crc = crc_init();
    crc = crc_update(crc, &data, 4);
    crc = crc_finalize(crc);
    if (crc == get_crc(shdr)) {
        return RB_OK;
    }
    return RB_BAD_HDR;
}

static rb_errors_t make_sector_header(rb_t *rb, rb_sector_header *shdr) {
    if (shdr == NULL || rb == NULL) {
        return RB_BAD_CALLER_DATA;
    }
    shdr->header = 0;
    set_index(shdr, ++rb->sector_index); //should range from 1 to 7fffff
    uint32_t data = get_index(shdr);
    crc_t crc = crc_init();
    crc = crc_update(crc, &data, 4);
    crc = crc_finalize(crc);
    set_crc(shdr, crc);
    return RB_OK;
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
static void nextincr(rb_t *rb, int incr) {
    rb->next += incr;
    if (rb->next >= rb->number_of_bytes) {
        rb->next = 0; //wrap to next sector
    }
}

/*
    tricky side effect. if at the start of a sector, will change rb->next;
*/
static rb_errors_t fetch_and_check_header(rb_t *rb, rb_header *phdr, int jumpto) {
    uint32_t nextoffs = (rb->next + jumpto);
    assert(nextoffs < rb->number_of_bytes);
    flash_read(rb->base_address + nextoffs, phdr, sizeof(*phdr));
    if (MOD_SECTOR(nextoffs) == 0) {
        //this is the start of a sector
        rb_errors_t t = is_sector_header_good((rb_sector_header *) phdr);
        if (t != RB_OK) {
            return t;
        }
        rb->next += sizeof(*phdr); //skip sector header, check data header
        flash_read(rb->base_address + rb->next + jumpto, phdr, sizeof(*phdr));
    }
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
        if (offs > rb->number_of_bytes) {
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
            if (rb->next >= rb->number_of_bytes) {
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
            if (hdr_res != RB_OK && hdr_res != RB_BLANK_HDR) {
                return hdr_res;
            }
        }
    } while (1);
    return hdr_res;
}
/*
 Search the sector ring for the oldest sector. If all sectors are blank, then it
 will start in the first sector. For example secA secB, is the ring, either
 could be the oldest. But we know the ring order is from first sector to last
 always and then wrap back to the first. If there are no erased sectors the
 sector with the lowest number will be the oldest. If there is an erased sector
 the one before it in the ring is the oldest (unless it is also blank, etc.).
*/
static rb_errors_t rb_find_ring_oldest_sector(rb_t *rb) {
    uint32_t oldnext = 0;
    rb_sector_header hdr;
    rb_errors_t hdr_res = RB_BAD_HDR;
    int oldest_sector_number = RB_INDEX_MASK; //largest possible index
    if (rb == NULL ) {
        return RB_BAD_CALLER_DATA; // Error handling: Null pointer passed
    }
    /*
        scan all sectors, return either the earliest blank sector or the oldest
        used.
        If sectors are (7 erased 8), oldest is 7.
        If sectors are (erased 9 10), oldest is 9.
        If sectors are (1 2 erased), oldest is 1.
        If all sectors are erased, oldest is the first sector
    */
    int offs = rb->number_of_bytes - FLASH_SECTOR_SIZE; //start on last sector
    do {
        rb->next = offs;
        flash_read(rb->base_address + rb->next, &hdr, sizeof(hdr));
        hdr_res = is_sector_header_good(&hdr);
        switch (hdr_res) {
        case RB_OK: //legit hdr, update ptrs, start here
            if (get_index(&hdr) < oldest_sector_number) {
                //lower indexes are always older
                oldest_sector_number = get_index(&hdr);
                oldnext = rb->next; //save ptr to oldest
            }
            break;
        case RB_BLANK_HDR: //header is erased, keep looking
            break;
        default:
            return hdr_res; //some error finding start
        }
        offs -= FLASH_SECTOR_SIZE;
    } while (offs >= 0);
    //we have searched the ring, no hdr found, so start at ring start
    rb->next = oldnext;
    return hdr_res;
}
// here I know entire write will be in this sector, maybe multiple pages
/*
  call with a block to write. If it fits in the page, fine write it. If not,
  write as much as will fit. return either negative error number of positive
  number of bytes still to be written in the next sector. Will flash write to
  flash memory either if fills a page or if flush is true.
*/
static int rb_partial(rb_t *rb, const void *data, uint32_t size, bool flush) {
    uint32_t pagerem = FLASH_PAGE_SIZE - MOD_PAGE(rb->next);
    uint32_t wrlen = MIN(pagerem, size); //amount I can write

    memcpy(&rb->rb_page[MOD_PAGE(rb->next)], data, wrlen);
    if (!MOD_PAGE(rb->next + wrlen) || flush) {
        //write buffered page into flash
        flash_prog(rb->base_address + FLASH_PAGE(rb->next), rb->rb_page, FLASH_PAGE_SIZE);
        memset(rb->rb_page, 0xff, FLASH_PAGE_SIZE); //get page buffer ready
    }
    rb->next += wrlen;
    if (rb->next >= rb->number_of_bytes) {
        rb->next = 0; //wrap to next sector
    }
    return size - wrlen; //amount not written
}
/*
 the problem is there are two things to be written, the hdr and the data either
 or both may be in the previously written page, then the rest can be written in
 the next page. So like a sector, first write the partial hdr and then the rest
 of the write which we know will fit in the sector.
*/
static rb_errors_t rb_append_page(rb_t *rb, const void *data, uint32_t size) {

    if (MOD_PAGE(rb->next)) {
        //need to load previously written data page aligned
        //fixme I don't think I ever need to read old data, just write 0xff to old bytes
        flash_read(rb->base_address + FLASH_PAGE(rb->next), rb->rb_page, FLASH_PAGE_SIZE);
    } else {
        //new page, no need to read current stuff
        memset(rb->rb_page, 0xff, FLASH_PAGE_SIZE);
    }
    // int remaining = rb_partial(rb, hdr, sizeof(*hdr), false);
    // if (remaining) {
    //     //header was split over a page, write rest to next page
    //     remaining = rb_partial(rb, hdr + sizeof(*hdr) - remaining, remaining, false);
    // }
    int remaining = rb_partial(rb, data, size, true);
    while (remaining) {
        //header was split over a page, write rest to next page
        remaining = rb_partial(rb, data + size - remaining, remaining, true);
    }
    return RB_OK;
}
/*
    helper to make page header, and occasionally also a sector header - and
    write it. Sequence to write anything.. first write sector hdr if necessary,
    then data hdr, then write data using another call to rb_append_page.
*/
static rb_errors_t write_headers(rb_t *rb, rb_header * hdr, uint32_t size, uint8_t flag) {
    rb_errors_t hdr_res;
    rb_sector_header rbsh;
    if (MOD_SECTOR(rb->next) == 0) {
        hdr_res = make_sector_header(rb, &rbsh);
        hdr_res = rb_append_page(rb, &rbsh, sizeof(rbsh));
        if (hdr_res != RB_OK) {
            return hdr_res;
        }
    }
    hdr_res = make_header(hdr, hdr->id, size);
    hdr->crc |= flag;       //set special flags in unused crc bits
    hdr_res = rb_append_page(rb, hdr, sizeof(*hdr));
    return hdr_res;
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
    // int id = hdr->id;
    int hdrsize = sizeof(*hdr);
    uint32_t size_needed = size + hdrsize;;
    if (rb == NULL || data == NULL || size == 0 || hdr == NULL ||
        hdr->id >= 0xff || size_needed > rb->number_of_bytes) {
        return RB_BAD_CALLER_DATA;
    }
    uint32_t blank_cnt = sector_blank_scan(rb);
    if (blank_cnt < size_needed) return RB_FULL;

    if (size_needed < FLASH_SECTOR_SIZE - MOD_SECTOR(rb->next)) {
        //write will fit this flash sector, write pages
        hdr_res = write_headers(rb, hdr, size, 0);
        if (hdr_res != RB_OK) {
            return hdr_res;
        }
        hdr_res = rb_append_page(rb, data, size);
    } else {
        //write will span two sectors, I assume current sector is good
        //fixme assume will not be more than 2 sectors.
        rb_header rbh2;
        uint32_t size_in_sector = FLASH_SECTOR_SIZE - MOD_SECTOR(rb->next) - hdrsize;
        uint32_t nextsector =  FLASH_SECTOR(rb->next) + FLASH_SECTOR_SIZE;
        if (nextsector >= rb->number_of_bytes) {
            nextsector = 0; //wrap to first sector allocated
        }
        //check header for next flash sector at start of sector
        uint32_t savenext = rb->next;
        hdr_res = fetch_and_check_header(rb, &rbh2, nextsector - rb->next);
        if (hdr_res == RB_BLANK_HDR) {
            //Only continue if next sector is blank, otherwise caller handles it.
            //first write current sector until filled
            hdr_res = write_headers(rb, hdr, size_in_sector, 0);
            if (hdr_res != RB_OK){
                return hdr_res;
            }
            //second write new sector header, split data header.
            hdr_res = write_headers(rb, hdr, size - size_in_sector, RB_HEADER_SPLIT);
            //fixme only works if write doesn't go into another sector
            hdr_res = rb_append_page(rb, data, size - size_in_sector);
        } else {
            // not enough space is available, let caller know so he can erase.
            rb->next = savenext;
            if(hdr_res == RB_OK) {
                return RB_WRAPPED_SECTOR_USED; //special error, we have wrapped, ring is full
            }
            return hdr_res;
        }
    }
    return hdr_res;
}
// every call will flash the involved sector(s), even tiny data
rb_errors_t rb_append(rb_t *rb, uint8_t id, const void *data, uint32_t size,
                      uint8_t *pagebuffer, bool erase_if_full) {
    rb_errors_t hdr_res;
    rb_header rbh;
    if (rb == NULL || data == NULL || size == 0 || id == 0xff || 
        pagebuffer == NULL || size > (rb->number_of_bytes - sizeof(rbh))) {
        return RB_BAD_CALLER_DATA;
    }

    rb->rb_page = pagebuffer; //set temp pointer
    //rbcreate and other appends guarantee pointers are good in rb
    hdr_res = rb_findnext_writeable(rb); //get pointers in rb
    if (hdr_res == RB_HDR_LOOP && erase_if_full) {
        hdr_res = rb_find_ring_oldest_sector(rb);
        if (hdr_res != RB_OK) {
            return hdr_res;
        }
        assert( MOD_SECTOR(rb->next) == 0);
        flash_erase(rb->base_address + rb->next, FLASH_SECTOR_SIZE);
    }
    hdr_res = rb_findnext_writeable(rb); //get pointers in rb
    if (hdr_res == RB_BLANK_HDR) {
        //fixme add sector header if first in sector
        hdr_res = make_header(&rbh, id, size);
        hdr_res = rb_sector_append(rb, &rbh, data, size); //fixme handle errors!
        if (hdr_res == RB_WRAPPED_SECTOR_USED) {
            hdr_res = rb_find_ring_oldest_sector(rb);
            assert( MOD_SECTOR(rb->next) == 0);
            flash_erase(rb->base_address + rb->next, FLASH_SECTOR_SIZE);
        }
    }
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
        size > (rb->number_of_bytes - sizeof(hdr))) {
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
        nextincr(rb, read_size); //fixme use this elsewhere...
        remaining_size -= read_size;
        data += read_size;
        while (remaining_size) {
            read_size = MIN(FLASH_SECTOR_SIZE, remaining_size);
            flash_read(rb->base_address + rb->next, data, read_size);
            rb->next += read_size;
            if (rb->next >= rb->number_of_bytes) {
                rb->next = 0; //wrap to next sector
            }
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
        hdr_err = rb_find_ring_oldest_sector(rb);
        if (hdr_err == RB_OK || hdr_err == RB_BLANK_HDR) {
            if (write_buffer) {
                hdr_err = rb_findnext_writeable(rb);
            }
        }
    }
    //it is up to the user to deal with rb errors
    return hdr_err;
}
