/*
 * Copyright 2024, Hiroyuki OYAMA. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "circular_buffer.h"
#include <math.h>


void cb_restore(cb_t *cb) {
    if (cb == NULL || cb->get_timestamp == NULL) {
        return; // Error handling: Null pointer passed
    }

    uint64_t oldest_timestamp = 0xffffffffffffffff;
    uint64_t newest_timestamp = 0;
    cb->is_full = false;

    void *entry = malloc(cb->item_size);
    if (entry == NULL) {
        printf("Out of memory\n");
        return;
    }

    size_t actual_length = cb->total_sectors * cb->pages_per_sector * cb->items_per_page;

    uint32_t n = 0;
    for (size_t i = 0; i < actual_length; i++) {
        uint32_t address = cb->address + (i * cb->item_size);
        flash_read(address, entry, cb->item_size);
        uint64_t timestamp = cb->get_timestamp(entry);
        if (timestamp == 0xffffffffffffffff || timestamp == 0) {
            continue;
        }
        if (timestamp > newest_timestamp) {
            newest_timestamp = timestamp;
            cb->head = i + 1;
        }
        if (timestamp < oldest_timestamp) {
            oldest_timestamp = timestamp;
            cb->tail = i;

        }

        n++;
        if (n > cb->length) {
            cb->is_full = true;
        }
    }
    free(entry);

    if (cb->is_full) {
        cb->tail = cb->head - cb->length;
    }
    if (oldest_timestamp == 0xffffffffffffffff) {
        cb->tail = 0;
    }
}

static void init_cb_status(cb_t *cb, uint32_t address, size_t length, size_t item_size,
                    timestamp_extractor_t get_timestamp)
{
    cb->get_timestamp = get_timestamp;
    cb->address = address;
    cb->length = length;
    cb->item_size = item_size;
    cb->head = 0;
    cb->tail = 0;
    cb->is_full = false;
    cb->items_per_page = FLASH_PAGE_SIZE / item_size;
    uint32_t total_pages_needed = ceil((float)length / cb->items_per_page);
    cb->pages_per_sector = FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE;
    cb->total_sectors = ceil((float)total_pages_needed / cb->pages_per_sector) + 1;
}

int cb_create(cb_t *cb, uint32_t address, size_t length, size_t item_size,
              timestamp_extractor_t get_timestamp, bool force_initialize) {
    if (cb == NULL || get_timestamp == NULL) {
        return -1;
    }
    if (item_size > FLASH_SECTOR_SIZE) {
        return -2;
    }
    if (item_size > FLASH_PAGE_SIZE) {
        return -3;
    }

    init_cb_status(cb, address, length, item_size, get_timestamp);

    bool is_invalid = false;  // have erased sector?
    if (force_initialize || is_invalid) {
        flash_erase(cb->address, cb->total_sectors * FLASH_SECTOR_SIZE);
    } else {
        cb_restore(cb);
    }

    return cb->total_sectors * FLASH_SECTOR_SIZE;
}

static void append_flash_memory(cb_t *cb, const void *data, size_t size) {
    // manage flash page boundary
    uint32_t page = floor((float)cb->head / cb->items_per_page);
    uint32_t position_in_page = cb->head % cb->items_per_page;
    uint32_t offset = position_in_page * cb->item_size;

    // Data is only an addendum, so no erase is needed for additions within this page
    uint8_t page_data[FLASH_PAGE_SIZE];
    flash_read(cb->address + page * FLASH_PAGE_SIZE, page_data, sizeof(page_data));
    memcpy(page_data + offset, data, size);
    flash_prog(cb->address + page * FLASH_PAGE_SIZE, page_data, sizeof(page_data));
}

static void erase_next_flash_sector_if_necessary(cb_t *cb) {
    // check flash sector boundary
    uint32_t page = floor((float)cb->head / cb->items_per_page);
    uint32_t sector = floor((float)page / cb->pages_per_sector);
    uint32_t next_sector = floor(((float)cb->head + 1) / cb->items_per_page) / cb->pages_per_sector;

    // Erase the adjacent sector to be processed next. This is where the oldest data is stored.
    if (sector != next_sector) {
        flash_erase(cb->address + (next_sector % cb->total_sectors) * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
    }
}

static void update_buffer_append_state(cb_t *cb) {
    size_t actual_length = cb->total_sectors * cb->pages_per_sector * cb->items_per_page;
    if (cb->is_full) {
        cb->head = (cb->head + 1) % actual_length;
        cb->tail = (cb->tail + 1) % actual_length;

    } else if (!cb->is_full && cb->head < cb->length) {
        cb->head = (cb->head + 1) % actual_length;
    } else {
        cb->head = (cb->head + 1) % actual_length;
        cb->tail = (cb->tail + 1) % actual_length;
        cb->is_full = true;
    }
}

void cb_append(cb_t *cb, const void *data, size_t size) {
    if (cb == NULL || data == NULL || size == 0) {
        return;
    }

    append_flash_memory(cb, data, size);
    erase_next_flash_sector_if_necessary(cb);
    update_buffer_append_state(cb);
}

void cb_open_cursor(cb_t *cb, cb_cursor_t *cursor, cb_cursor_order_t order) {
    if (cursor == NULL || cb == NULL) {
        return;
    }

    cursor->cb = cb;
    cursor->order = order;
    if (order == RB_CURSOR_DESCENDING) {
        cursor->index = cb->head > 0 ? cb->head - 1 : cb->length - 1;
    } else {
        cursor->index = cb->tail;
    }
}

static bool check_end_of_cursor(cb_cursor_t *cursor) {
    cb_t *cb = cursor->cb;
    uint32_t next_tail = cb->tail > 0 ? cb->tail - 1 : 0;

    if (cb->is_full && cursor->order == RB_CURSOR_DESCENDING && cursor->index == next_tail) {
        return false;
    } else if (!cb->is_full && cursor->order == RB_CURSOR_DESCENDING && cursor->index == 0) {
        return false;
    } else if (cb->is_full && cursor->order == RB_CURSOR_ASCENDING && cursor->index == cb->head) {
        return false;
    } else if (!cb->is_full && cursor->order == RB_CURSOR_ASCENDING && cursor->index == cb->head) {
        return false;
    }
    return true;
}

static void update_cursor_status(cb_cursor_t *cursor) {
    cb_t *cb = cursor->cb;
    size_t actual_length = cb->total_sectors * cb->pages_per_sector * cb->items_per_page;

    if (cursor->order == RB_CURSOR_DESCENDING) {
        if (cursor->index == 0) {
            cursor->index = actual_length - 1;
        } else {
            cursor->index--;
        }
    } else {
        cursor->index = (cursor->index + 1) % actual_length;
    }
}

bool cb_get_next(cb_cursor_t *cursor, void *entry) {
    if (cursor == NULL || cursor->cb == NULL) {
        return false;
    }
    if (!check_end_of_cursor(cursor)) {
        return false;
    }

    cb_t *cb = cursor->cb;
    uint32_t address = cb->address + cursor->index * cb->item_size;
    flash_read(address, entry, cb->item_size);

    update_cursor_status(cursor);
    return true;
}
