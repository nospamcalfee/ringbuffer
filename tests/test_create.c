#include "tests.h"

#define CIRCULAR_BUFFER_LENGTH  10

typedef struct {
    uint64_t timestamp;
    uint16_t value;
} test_item_t;

static uint64_t get_timestamp(void *obj) {
    test_item_t *item = (test_item_t *)obj;
    return item->timestamp;
}

static void setup(void) {
    const size_t erase_size = ((CIRCULAR_BUFFER_LENGTH * sizeof(test_item_t) + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;
    flash_erase(FLASH_BASE, erase_size);
}

static void cleanup(void) {
    setup();
}

static void test_create_simple(void) {
    cb_t cb;

    setup();

    cb_create(&cb, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(test_item_t), &get_timestamp, true);
    assert(cb.length == CIRCULAR_BUFFER_LENGTH);
    assert(cb.item_size == sizeof(test_item_t));
    assert(cb.head == 0);
    assert(cb.tail == 0);
    assert(cb.is_full == false);

    cleanup();
}

static void test_storage_size(void) {
    cb_t cb;
    int created_size;

    setup();

    created_size = cb_create(&cb, FLASH_BASE, 2, sizeof(test_item_t), &get_timestamp, true);
    assert(created_size == 2*4096);

    created_size = cb_create(&cb, FLASH_BASE, 256, sizeof(test_item_t), &get_timestamp, true);
    assert(created_size == 2*4096);
    created_size = cb_create(&cb, FLASH_BASE, 256 + 1, sizeof(test_item_t), &get_timestamp, true);
    assert(created_size == 3*4096);

    created_size = cb_create(&cb, FLASH_BASE, 2, 4096+1, &get_timestamp, true);
    assert(created_size == -2);
    created_size = cb_create(&cb, FLASH_BASE, 2, 256+1, &get_timestamp, true);
    assert(created_size == -3);

    cleanup();
}


void test_create(void) {
    printf("create ...................");

    test_create_simple();
    test_storage_size();

    printf("ok\n");
}
