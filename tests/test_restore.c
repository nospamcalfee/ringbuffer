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

static void test_restore_1(void) {
    cb_t cb_first, cb_after;
    test_create_item_t item;

    setup();

    cb_create(&cb_first, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(test_create_item_t), &get_timestamp, true);
    item.timestamp = 1;
    item.value = 0x1234;
    cb_append(&cb_first, &item, sizeof(item));

    // shutdown and restart

    cb_create(&cb_after, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(test_create_item_t), &get_timestamp, false);
    assert(cb_after.head == 1);
    assert(cb_after.tail == 0);
    assert(cb_after.is_full == false);

    cleanup();
}

static void test_restore_full(void) {
    cb_t cb_first, cb_after;
    test_create_item_t item;

    setup();

    cb_create(&cb_first, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(test_create_item_t), &get_timestamp, true);
    for (int i = 0; i < CIRCULAR_BUFFER_LENGTH; i++) {
        item.timestamp = i + 1;
        item.value = 0x1234 + i;
        cb_append(&cb_first, &item, sizeof(item));
    }

    // shutdown and restart

    cb_create(&cb_after, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(test_create_item_t), &get_timestamp, false);
    assert(cb_after.head == 0);
    assert(cb_after.tail == 1);
    assert(cb_after.is_full == true);

    cleanup();
}


void test_restore(void) {
    printf("restore ..................");

    test_restore_1();
    test_restore_full();

    printf("ok\n");
}
