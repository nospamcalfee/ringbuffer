#include "circular_buffer.h"


static const uint32_t MAGIC = 0xABCD1234;

void cb_restore(cb_t *cb) {
    if (cb == NULL || cb->get_timestamp == NULL) {
        return; // Error handling: Null pointer passed
    }

    uint64_t oldest_timestamp = 0xffffffffffffffff;
    uint64_t newest_timestamp = 0;
    size_t latest_head = 0;
    bool is_valid_entry_found = false;
    size_t num_entries = cb->length;

    for (size_t i = 0; i < cb->length; i++) {
        uint32_t address = cb->address + sizeof(MAGIC) + (i * cb->item_size);
        uint8_t entry[cb->item_size];
        flash_read(address, entry, cb->item_size);
        uint64_t timestamp = cb->get_timestamp((void *)entry);
        if (timestamp == 0xffffffffffffffff || timestamp == 0) {
            continue;
        }
        if (timestamp > newest_timestamp) {
            newest_timestamp = timestamp;
            cb->head = i;
        }
        if (timestamp < oldest_timestamp) {
            oldest_timestamp = timestamp;
            cb->tail = i;
        }
    }

    if (oldest_timestamp == 0xffffffffffffffff) {
        cb->tail = 0;
    }
}

void cb_create(cb_t *cb, uint32_t address, size_t length, size_t item_size,
               timestamp_extractor_t get_timestamp, bool force_initialize) {
    if (cb == NULL || get_timestamp == NULL) {
        return;
    }

    cb->address = address;
    cb->length = length;
    cb->item_size = item_size;
    cb->head = 0;
    cb->tail = 0;
    cb->is_full = false;
    cb->_total_size = (( sizeof(MAGIC) + item_size * length + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;
    if ((address % FLASH_SECTOR_SIZE) != 0) {
        cb->address = ((address / FLASH_SECTOR_SIZE) + 1) * FLASH_SECTOR_SIZE;
    }
    uint8_t buffer[FLASH_PAGE_SIZE] = {0};
    ((uint32_t *)buffer)[0] = MAGIC;

    if (force_initialize) {
        flash_erase(cb->address, FLASH_SECTOR_SIZE);
        flash_prog(cb->address, buffer, FLASH_PAGE_SIZE);
    } else {
        uint32_t magic;
        flash_read(cb->address, &magic, sizeof(magic));
        if (magic != MAGIC) {
            flash_erase(cb->address, FLASH_SECTOR_SIZE);
            flash_prog(cb->address, buffer, FLASH_PAGE_SIZE);
        } else {
            cb_restore(cb);
        }
    }
}

void cb_append(cb_t *cb, const void *data, size_t size) {
    if (cb == NULL || data == NULL || size == 0) {
        return;
    }

    uint32_t write_address = cb->address + sizeof(MAGIC) + (cb->head * cb->item_size) % cb->_total_size;
    // Align the write address to the nearest lower 256-byte boundary for flash programming
    uint32_t block_start_address = write_address & ~(FLASH_SECTOR_SIZE - 1);

    uint8_t sector_update[FLASH_SECTOR_SIZE];
    flash_read(block_start_address, sector_update, FLASH_SECTOR_SIZE);
    size_t offset = write_address - block_start_address;
    memcpy(sector_update + offset, data, size);
    flash_erase(block_start_address, FLASH_SECTOR_SIZE);
    flash_prog(block_start_address, sector_update, FLASH_SECTOR_SIZE);

    if (cb->is_full) {
        cb->tail = (cb->tail + 1) % cb->length;
        cb->head = (cb->head + 1) % cb->length;
    } else if (!cb->is_full && cb->head < cb->length - 1) {
        cb->head = (cb->head + 1) % cb->length;
    } else {
        cb->head = (cb->head + 1) % cb->length;
        cb->tail = (cb->head + 1) % cb->length;
        cb->is_full = true;
    }
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

bool cb_get_next(cb_cursor_t *cursor, void *entry) {
    if (cursor == NULL || cursor->cb == NULL) {
        return false;
    }
    cb_t *cb = cursor->cb;
    uint32_t next_tail = cb->tail > 0 ? cb->tail - 1 : cb->length - 1;
    if (cb->is_full && cursor->order == RB_CURSOR_DESCENDING && cursor->index == next_tail) {
        return false;
    } else if (!cb->is_full && cursor->order == RB_CURSOR_DESCENDING && cursor->index == 0) {
        return false;
    } else if (cb->is_full && cursor->order == RB_CURSOR_ASCENDING && cursor->index == next_tail) {
        return false;
    } else if (!cb->is_full && cursor->order == RB_CURSOR_ASCENDING && cursor->index == cb->head) {
        return false;
    }

    uint32_t address = cb->address + sizeof(MAGIC) + cursor->index * cb->item_size;
    flash_read(address, entry, cb->item_size);

    if (cursor->order == RB_CURSOR_DESCENDING) {
        if (cursor->index == 0) {
            cursor->index = cb->length - 1;
        } else {
            cursor->index--;
        }
    } else {
        cursor->index = (cursor->index + 1) % cb->length;
    }
    return true;
}
