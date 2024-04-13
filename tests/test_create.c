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


void test_create(void) {
    cb_t cb;

    setup();

    printf("create ...................");
    cb_create(&cb, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(test_create_item_t), &get_timestamp, true);

    assert(cb.length == CIRCULAR_BUFFER_LENGTH);
    assert(cb.item_size == sizeof(test_create_item_t));
    assert(cb.head == 0);
    assert(cb.tail == 0);
    assert(cb.is_full == false);

    printf("ok\n");

    cleanup();
}
