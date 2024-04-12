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
    size_t _total_size;
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
void cb_create(cb_t *cb, uint32_t address, size_t length, size_t item_size,
               timestamp_extractor_t get_timestamp, bool force_initialize);

void cb_open_cursor(cb_t *cb, cb_cursor_t *cursor, cb_cursor_order_t order);
bool cb_get_next(cb_cursor_t *cursor, void *data);
void cb_close_cursor(cb_cursor_t *cursor);
void cb_append(cb_t *cb, const void *data, size_t size);

#endif
