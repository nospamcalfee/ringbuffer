/*
 * Copyright 2024, Steve Calfee. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 * Idea spawned from circular_buffer app
 * Copyright 2024, Hiroyuki OYAMA. All rights reserved.
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
    if ((rbh->crc & ~(RB_HEADER_SPLIT |
                      RB_HEADER_NOT_SMUDGED |
                      RB_HEADER_UNUSED)) != crc) {
        return RB_BAD_HDR;
    }
    return RB_OK; //for now no crc check
}
static rb_errors_t is_header_good(rb_header *rbh) {
    if (rbh->id == 0xff && rbh->crc == 0xff && rbh->len == RB_MAX_LEN_VALUE) {
        return RB_BLANK_HDR;
    }
    if (rbh->id == 0xff || rbh->len == 0 || rbh->len > __PERSISTENT_LEN) {
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
    // should range from 1 to 7fffff fixme analyze possible range with respect
    // to nand flash life
    set_index(shdr, ++rb->sector_index);
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
 sector but there is no room for at least a header and one data byte, skip to
 next sector. Irregardless of len argument the max it will increment is to the
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
        if (offs >= rb->number_of_bytes) {
            offs = 0; //wrap around flash allocation
        }
        uint32_t nextblanks = count_blanks((uint8_t *)XIP_NOCACHE_NOALLOC_BASE + rb->base_address + offs, 0xff, FLASH_SECTOR_SIZE);
        blanks += nextblanks;
    } 
    return blanks;
}
/* 
find next writeable flash offset. start at current rb flash sector, looking for
valid data, update rb ring ptrs. Return error code, side effect, update ring
pointers. Writes will always leave at least room for one more hdr and will fail
if there is not enough room.
*/
static rb_errors_t rb_findnext_writeable(rb_t *rb) {
    rb_header hdr;
    uint32_t origrb = rb->next; //start of this page
    assert(rb->next < rb->number_of_bytes);
    rb_errors_t hdr_res;
    if (rb == NULL ) {
        return RB_BAD_CALLER_DATA; // Error handling: Null pointer passed
    }
    do {
        if (MOD_SECTOR(rb->next) > FLASH_SECTOR_SIZE - sizeof(hdr) - 1) {
            //skip last few bytes of sector
            nextincr(rb, FLASH_SECTOR_SIZE - MOD_SECTOR(rb->next));
        }
        hdr_res = fetch_and_check_header(rb, &hdr, 0); //fetch and check header
        if (hdr_res != RB_OK) {
            break; //return hdr_res; //return errors and RB_BLANK_HDR here
         }
        //found a good header, use it to skip ahead
        //keep looking for end of rb
        rb->next = rb_incr(rb->next, hdr.len + sizeof(hdr), rb->number_of_bytes);
//fixme in a single sector system can I detect  a full sector? the following 
//worked for multi sectors.
        if (rb->next == origrb){
            //data in flash is full, we wrapped.
            rb->next = FLASH_SECTOR(rb->next);
            return RB_HDR_LOOP;
        }
    } while (1);
    return hdr_res;
}
/*
 Search the sector ring for the oldest sector. If all sectors are blank, then it
 will start in the first sector. For example secA secB, is the ring, either
 could be the oldest. But we know the ring order is from first sector to last
 always and then wrap back to the first. If there are no erased sectors the
 sector with the lowest number will be the oldest.
*/
static rb_errors_t rb_find_ring_oldest_sector(rb_t *rb) {
    uint32_t oldnext = 0;
    rb_sector_header hdr;
    rb_errors_t hdr_res = RB_BAD_HDR;
    uint32_t oldest_sector_number = RB_INDEX_MASK; //largest possible index
    if (rb == NULL ) {
        return RB_BAD_CALLER_DATA; // Error handling: Null pointer passed
    }
    /*
        scan all sectors, return either the earliest blank sector or the oldest
        used.
        If sectors are (7 erased 8), oldest is 7, but badly ordered
        If sectors are (8 erased 7), oldest is 7
        If sectors are (erased 9 10), oldest is 9.
        If sectors are (1 2 erased), oldest is 1.
        If no sectors are erased (3 1 2), oldest is 1 (lowest)
        If all sectors are erased, oldest is the first sector
        Erased sectors are always assumed grouped, generally only 1 sector
        unless entire flash was erased.
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
            if (get_index(&hdr) >= rb->sector_index) {
                //update largest index for new sector creation
                rb->sector_index = get_index(&hdr);
            }
            break;
        case RB_BLANK_HDR: //header is erased
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
/*
 check entire flash for reasonable order. ie oldest < next < nextnext etc, with
 any sector startinq at blank, is followed by other sectors starting at blank.
 Modulo the ring size.
*/
rb_errors_t rb_check_sector_ring(rb_t *rb) {
    int blankcount = 0;
    rb_errors_t check_status = RB_OK;
    rb_sector_header hdr;
    rb_errors_t hdr_res;
    uint32_t oldest_sector_number = RB_INDEX_MASK; //largest possible index
    uint32_t last_blank_sector = 0;

    //first just check the sector headers, if bad, we will erase everything
    //back through all sectors
    // for (int i = rb->number_of_bytes - FLASH_SECTOR_SIZE; i >= 0 ; i -= FLASH_SECTOR_SIZE) {
    for (uint32_t i = 0; i < rb->number_of_bytes; i += FLASH_SECTOR_SIZE) {
        rb->next = i;
        flash_read(rb->base_address + rb->next, &hdr, sizeof(hdr));
        hdr_res = is_sector_header_good(&hdr);
        if (hdr_res == RB_OK) {
            if (get_index(&hdr) < oldest_sector_number) {
                //lower indexes are always older
                oldest_sector_number = get_index(&hdr);
            }
            if (get_index(&hdr) >= rb->sector_index) {
                //update largest index for new sector creation
                rb->sector_index = get_index(&hdr);
            }
        } else {
            if (hdr_res == RB_BLANK_HDR) {
                blankcount++;
                last_blank_sector = i; //last erased in ring
            } else {
                //some bad error
                check_status = RB_BAD_HDR;
            }
        }
    }
    //now starting at the last erased flash sector, go through flash again,
    //insure sectors increase
    if (blankcount == 0) {
        hdr_res = rb_find_ring_oldest_sector(rb);
        //should never be an error here
        last_blank_sector = rb->next; //where the ring should start
    }
    uint32_t low = 0;
    for (uint32_t i = 0; i < rb->number_of_bytes && check_status == RB_OK; i += FLASH_SECTOR_SIZE) {
        rb->next = i + last_blank_sector;
        if (rb->next > rb->number_of_bytes) {
            rb->next -= rb->number_of_bytes; //wrap in ring buffer
        }
        flash_read(rb->base_address + rb->next, &hdr, sizeof(hdr));
        hdr_res = is_sector_header_good(&hdr);
        if (hdr_res == RB_OK) {
            if (get_index(&hdr) < low) {
                check_status = RB_BAD_HDR; //exit loop
            }
            low = get_index(&hdr); //move up low counter
        } else {
            //only non-ok is blank here
            break;
        }
    }
    //caller can erase if desired
    // if (check_status != RB_OK) {
    //     //something is wrong, erase the whole flash area
    //     printf("!!!!!!!!initing flash addr 0x%lx, len 0x%lx\n", rb->base_address, rb->number_of_bytes);
    //     flash_erase(rb->base_address, rb->number_of_bytes);
    //     return check_status;
    // }
    return check_status;
}
/*
  Here I know entire write will be in this sector, maybe multiple pages. call
  with a block to write. If it fits in the page, fine write it. If not, write
  as much as will fit. return either negative error number of positive number
  of bytes still to be written in the next sector. Will flash write to flash
  memory either if fills a page or if flush is true.
*/
static int rb_partial(rb_t *rb, const void *data, uint32_t size) {
    uint32_t pagerem = FLASH_PAGE_SIZE - MOD_PAGE(rb->next);
    uint32_t wrlen = MIN(pagerem, size); //amount I can write

    memcpy(&rb->rb_page[MOD_PAGE(rb->next)], data, wrlen);
    //write buffered page into flash
    flash_prog(rb->base_address + FLASH_PAGE(rb->next), rb->rb_page, FLASH_PAGE_SIZE);
    memset(rb->rb_page, 0xff, FLASH_PAGE_SIZE); //get page buffer ready
    nextincr(rb, wrlen);
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
        // flash_read(rb->base_address + FLASH_PAGE(rb->next), rb->rb_page, FLASH_PAGE_SIZE);
        memset(rb->rb_page, 0xff, FLASH_PAGE_SIZE);
    } else {
        //new page, no need to read current stuff
        memset(rb->rb_page, 0xff, FLASH_PAGE_SIZE);
    }
    int remaining = rb_partial(rb, data, size);
    while (remaining) {
        //header was split over a page, write rest to next page
        remaining = rb_partial(rb, data + size - remaining, remaining);
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
    rb->last_wrote = rb->next; //info to caller
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
        hdr_res = write_headers(rb, hdr, size, RB_HEADER_NOT_SMUDGED);
        if (hdr_res != RB_OK) {
            return hdr_res;
        }
        hdr_res = rb_append_page(rb, data, size);
    } else {
        //write will span two sectors, I assume current sector is good
        //fixme assumes will not be more than 2 sectors.
        rb_header rbh2;
        uint32_t size_in_first_sector = FLASH_SECTOR_SIZE - MOD_SECTOR(rb->next) - hdrsize;
        size_in_first_sector = MIN(size_in_first_sector, size);
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
            hdr_res = write_headers(rb, hdr, size_in_first_sector, RB_HEADER_NOT_SMUDGED);
            if (hdr_res != RB_OK){
                return hdr_res;
            }
            hdr_res = rb_append_page(rb, data, size_in_first_sector);
            if (hdr_res != RB_OK){
                return hdr_res;
            }
            //second write next sector header, split data header.
            uint32_t size_in_second_sector = size - size_in_first_sector;
            size_in_second_sector = MIN(size_in_second_sector,
                                        FLASH_SECTOR_SIZE - MOD_SECTOR(rb->next) - hdrsize);

            hdr_res = write_headers(rb, hdr, size_in_second_sector,
                                    RB_HEADER_SPLIT | RB_HEADER_NOT_SMUDGED);
            //write second sector.
            hdr_res = rb_append_page(rb, data + size_in_second_sector, size_in_second_sector);
            if (size - size_in_first_sector - size_in_second_sector > 0) {
                //recurse and write remainder fixme will this work for writes greater than sector size?
                hdr_res = rb_sector_append(rb, hdr,
                            data + size_in_second_sector + size_in_first_sector,
                            size - size_in_second_sector - size_in_first_sector);
                if (hdr_res != RB_OK) {
                    return hdr_res;
                }
            }
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
    uint32_t oldnext = rb->next;
    //rbcreate and other appends guarantee pointers are good in rb
    do {
        hdr_res = rb_find_ring_oldest_sector(rb);
        if (!(hdr_res == RB_OK || hdr_res == RB_BLANK_HDR)) {
            return hdr_res;
        }
        hdr_res = rb_findnext_writeable(rb); //get pointers in rb
        if (hdr_res == RB_HDR_LOOP && erase_if_full) {
            // nextincr(rb, FLASH_SECTOR_SIZE); //bump to next in ring
            rb_find_ring_oldest_sector(rb);
            // rb->next = FLASH_SECTOR(rb->next);
            flash_erase(rb->base_address + rb->next, FLASH_SECTOR_SIZE);
            hdr_res = RB_BLANK_HDR;
        }
        if (hdr_res == RB_BLANK_HDR) {
            rbh.id = id; //only thing needed from here on the header
            hdr_res = rb_sector_append(rb, &rbh, data, size);
            if ((hdr_res == RB_WRAPPED_SECTOR_USED || hdr_res == RB_FULL) && erase_if_full) {
                //erase next sector in ring
                // uint32_t offs = FLASH_SECTOR(rb->next) + FLASH_SECTOR_SIZE;
                // if (offs >= rb->number_of_bytes) {
                //     offs = 0; //wrap around flash allocation
                // }
                rb_find_ring_oldest_sector(rb);
                flash_erase(rb->base_address + rb->next, FLASH_SECTOR_SIZE);
                continue; //try append again
            }
        }
        break; //done with loop
    } while (1);
    rb->next = oldnext;
    return hdr_res;
}
/* search flash for an existing entry id and data match. return positive offset
   of match or negative error number. scratch must be at least size bytes
   longs */
int rb_find(rb_t *rb, uint8_t id, const void *data, uint32_t size, uint8_t *scratch) {
    rb_errors_t hdr_res;
    rb_header hdr;
    uint32_t orignext = FLASH_SECTOR(rb->next); //save start of search
    if (rb == NULL || data == NULL || size == 0 || id == 0xff || id == 00 ||
        size > (rb->number_of_bytes - sizeof(hdr))) {
        return -RB_BAD_CALLER_DATA;
    }
    do {
        hdr_res = fetch_and_check_header(rb, &hdr, 0); //fetch and check header
        if (hdr_res != RB_OK) {
            return -hdr_res; //return errors here
        }
        if (hdr.id != id || !(hdr.crc & RB_HEADER_NOT_SMUDGED)) {
            //not my data, or it was erased, skip the header and the data
            rb->next = rb_incr(rb->next, hdr.len + sizeof(hdr), rb->number_of_bytes);
            if (orignext == rb->next) return -RB_HDR_ID_NOT_FOUND;
            continue; //do loop again
        }
        //found next entry, read it into scratch buffer
        uint32_t oldnext = rb->next;
        // rb->next += sizeof(hdr);
        hdr_res = rb_read(rb, id, scratch, size);
        if (hdr_res > 0) {
            // now see if it is the correct entry, and found.
            if (!memcmp(data, scratch, size)) {
                return oldnext; //found match, return its location
            }
            continue; //keep searching.
        } else {
            return hdr_res;
        }
    } while (true);
}
//this function effectively deletes a flash record, by smudging it, which can be
//done after it is already written. Due to nand flash implementations, I can
//write 1 bits to 0 bits, but not vice-versa
static rb_errors_t rb_smudge(rb_t *rb, uint32_t offset_to_smudge) {
    rb_header hdr;
    uint32_t savenext = rb->next;
    rb->next = offset_to_smudge;
    rb_errors_t hdr_res = fetch_and_check_header(rb, &hdr, 0); //fetch and check header
    if (hdr_res != RB_OK) {
        return hdr_res; //return errors here
    }
    //overwrite the old crc byte clearing the smudge bit
    hdr.crc &= ~RB_HEADER_NOT_SMUDGED;
    rb->next += offsetof(rb_header, crc);
    printf("rb_smudge erasing 0x%lx\n", rb->next);
    int res = rb_append_page(rb, &hdr.crc, 1);
    rb->next = savenext; //fixme I am not sure I need this, but it shouldn't hurt.
    return res;
}
/* given a writable page, delete a matching id, string entry */
rb_errors_t rb_delete(rb_t *rb, uint8_t id, const void *data, uint32_t size, uint8_t *pagebuffer) {
    if (rb == NULL || data == NULL || id == 0 || id == 0xff ||
        pagebuffer == NULL) {
        return RB_BAD_CALLER_DATA;
    }
    rb->rb_page = pagebuffer; //set temp area pointer
    uint32_t oldnext = rb->next;
    //I think it makes sense to always delete the first match?
    rb_errors_t hdr_err = rb_find_ring_oldest_sector(rb);
    if (hdr_err != RB_OK) {
        return hdr_err;
    }
    int res = rb_find(rb, id, data, size, pagebuffer);
    if (res < 0) {
        //some error
        printf("some find failure %d looking for \"%s\"\n", -res, (char *) data);
    } else {
        printf("rb_delete erasing at 0x%lx\n%s\n", rb->next, (char *) data);
        res = rb_smudge(rb, res); //this deletes the entry
    }
    rb->next = oldnext;
    return -res;
}
/*
    read up to size data bytes into data buffer, of next flash which matches id.
    If data is less than size, check to see if split into two sectors, and add
    the second data to the read. If data is less than size, return only the
    actual data. Return actual amount read or a negative status code.
*/
int rb_read(rb_t *rb, uint8_t id, void *data, uint32_t size) {
    rb_errors_t hdr_res;
    rb_header hdr;
    int total_read = 0;
    uint32_t remaining_size = size;
    uint32_t orignext = FLASH_SECTOR(rb->next); //save start of search
    if (rb == NULL || data == NULL || size == 0 || id == 0xff || id == 00 ||
        size > (rb->number_of_bytes - sizeof(hdr))) {
        return -RB_BAD_CALLER_DATA;
    }
    do {
        hdr_res = fetch_and_check_header(rb, &hdr, 0); //fetch and check header
        if (hdr_res != RB_OK) {
            return -hdr_res; //return errors here
        }
        if (hdr.id != id || !(hdr.crc & RB_HEADER_NOT_SMUDGED)) {
            //not my data, or it was erased, keep looking
            rb->next = rb_incr(rb->next, hdr.len + sizeof(hdr), rb->number_of_bytes);
            if (orignext == rb->next) return -RB_HDR_ID_NOT_FOUND;
            continue; //do loop again
        }
        //found a good header, use it to read data, maybe split into two reads
        uint32_t read_size = MIN(hdr.len, remaining_size);
        rb->next += sizeof(hdr);
        // read data in current sector
        flash_read(rb->base_address + rb->next, data, read_size);
        // skip data so far, may be longer than amount just red
        rb->next = rb_incr(rb->next, hdr.len, rb->number_of_bytes);
        remaining_size -= read_size;
        data += read_size;
        total_read += read_size;
        if (!MOD_SECTOR(rb->next)) {
            //if we end on a sector boundary, maybe this data is split into the
            //new sector - this needs big testing...
            rb_errors_t prefetch_hdr_res = fetch_and_check_header(rb, &hdr, 0); //fetch and check header
            if ( !(prefetch_hdr_res == RB_OK || prefetch_hdr_res == RB_BLANK_HDR)) {
                return -prefetch_hdr_res;
            } else {
                //fixme do I need to check here for deleted? what about a split
                //deleted? should a smudged deleted also handle its split? I
                //think yes.
                if (hdr.id == id && (hdr.crc & RB_HEADER_SPLIT)) {
                    // I have peeked ahead and this data is split into the next
                    // sector, so recurse to read rest of it.
                    int res = rb_read(rb, id, data, remaining_size);
                    if (res > 0) {
                        total_read += res; //fixme - this only will work once
                    }
                }
            }
        }
        break;
    } while (1);
    //If returned data is too short return actual size
    return total_read;
}
/* 
Create a new variable sized ringbuffer control block, maybe erasing the whole
thing. Can be called at any time to re-init. Every init points to the oldest
sector and first item in the sector. So for reads it is like a rewind. Appends
will go to the end of the ring always.
*/
rb_errors_t rb_create(rb_t *rb, uint32_t base_address, 
                      size_t number_of_sectors, enum init_choices init_choice) {
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

    if (init_choice == CREATE_INIT_ALWAYS) {
        printf("************initing flash addr 0x%lx, len 0x%lx\n", rb->base_address, rb->number_of_bytes);
        flash_erase(rb->base_address, rb->number_of_bytes);
        hdr_err = RB_OK;
    } else {
        /* Request was to continue in rb as exists in flash. First verify flash
           is in reasonable order. */
        hdr_err = rb_check_sector_ring(rb);
        if (hdr_err == RB_OK) {
            //then set the pointer
            hdr_err = rb_find_ring_oldest_sector(rb);
        }
    }
    //it is up to the user to deal with rb errors
    return hdr_err;
}
//helper to create and re-create (if data is bad) a buffer control block
rb_errors_t rb_recreate(rb_t *rb, uint32_t base_address,
                            size_t number_of_sectors, enum init_choices init_choice) {
    rb_errors_t err = rb_create(rb, base_address, number_of_sectors, init_choice);
    if (init_choice != CREATE_FAIL) {
        if (!(err == RB_OK || err == RB_BLANK_HDR || err == RB_HDR_LOOP)) {
            printf("*****************starting flash error %d, reiniting\n", err);
            err = rb_create(rb, base_address, number_of_sectors, CREATE_INIT_ALWAYS);
            if (err != RB_OK) {
                //init failed, bail
                printf("starting flash error %d, quitting\n", err);
            }
        }
    }
    return err;
}
