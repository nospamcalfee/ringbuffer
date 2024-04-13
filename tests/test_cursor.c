#include "tests.h"

#define CIRCULAR_BUFFER_LENGTH  10

typedef struct {
    uint64_t timestamp;
    uint16_t value;
} test_create_item_t;

static uint64_t get_timestamp(void *obj) {
    test_create_item_t *item = (test_create_item_t *)obj;
    return item->timestamp;
}


static void setup(void) {
    const size_t erase_size = ((CIRCULAR_BUFFER_LENGTH * sizeof(test_create_item_t) + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;
    flash_erase(FLASH_BASE, erase_size);
}

static void cleanup(void) {
    setup();
}

static void test_cursor_descending(void) {
    cb_t cb;
    test_create_item_t item;

    setup();

    cb_create(&cb, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(test_create_item_t), &get_timestamp, true);
    for (int i = 0; i < CIRCULAR_BUFFER_LENGTH; i++) {
        item.timestamp = 1 + i;
        item.value = 0x1234 + i;
        cb_append(&cb, &item, sizeof(item));
    }

    cb_cursor_t cursor;
    cb_open_cursor(&cb, &cursor, RB_CURSOR_DESCENDING);
    for (int i = CIRCULAR_BUFFER_LENGTH - 1; i > 0; i--) {
        assert(cb_get_next(&cursor, &item) == true);
        assert(item.timestamp == (uint64_t)(1 + i));
        assert(item.value == 0x1234 + i);
    }
    assert(cb_get_next(&cursor, &item) == false);

    cleanup();
}

static void test_cursor_ascending(void) {
    cb_t cb;
    test_create_item_t item;

    setup();

    cb_create(&cb, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(test_create_item_t), &get_timestamp, true);
    for (int i = 0; i < CIRCULAR_BUFFER_LENGTH; i++) {
        item.timestamp = 1 + i;
        item.value = 0x1234 + i;
        cb_append(&cb, &item, sizeof(item));
    }

    cb_cursor_t cursor;
    cb_open_cursor(&cb, &cursor, RB_CURSOR_ASCENDING);
    for (int i = 1; i < CIRCULAR_BUFFER_LENGTH; i++) {
        assert(cb_get_next(&cursor, &item) == true);
        assert(item.timestamp == (uint64_t)(1 + i));
        assert(item.value == 0x1234 + i);
    }
    assert(cb_get_next(&cursor, &item) == false);

    cleanup();
}

void test_cursor(void) {
    printf("cursor ...................");

    test_cursor_descending();
    test_cursor_ascending();
    printf("ok\n");
}
