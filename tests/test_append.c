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


static void test_append1(void) {
    cb_t cb;
    test_create_item_t item;

    setup();

    cb_create(&cb, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(test_create_item_t), &get_timestamp, true);

    item.timestamp = 1;
    item.value = 0x1234;
    cb_append(&cb, &item, sizeof(item));
    assert(cb.head == 1);
    assert(cb.tail == 0);
    assert(cb.is_full == false);

    cleanup();
}

static void test_append9(void) {
    cb_t cb;
    test_create_item_t item;

    setup();

    cb_create(&cb, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(test_create_item_t), &get_timestamp, true);
    for (int i = 0; i < CIRCULAR_BUFFER_LENGTH - 1; i++) {
        item.timestamp = 1 + i;
        item.value = 0x1234 + i;
        cb_append(&cb, &item, sizeof(item));
    }

    assert(cb.head == CIRCULAR_BUFFER_LENGTH - 1);
    assert(cb.tail == 0);
    assert(cb.is_full == false);

    cleanup();
}

static void test_append_circular(void) {
    cb_t cb;
    test_create_item_t item;

    setup();

    cb_create(&cb, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(test_create_item_t), &get_timestamp, true);
    for (int i = 0; i < CIRCULAR_BUFFER_LENGTH; i++) {
        item.timestamp = 1 + i;
        item.value = 0x1234 + i;
        cb_append(&cb, &item, sizeof(item));
    }

    assert(cb.head == 0);
    assert(cb.tail == 1);
    assert(cb.is_full == true);

    cleanup();
}

static void test_persistent_value(void) {
    cb_t cb;
    test_create_item_t item;

    setup();

    cb_create(&cb, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(test_create_item_t), &get_timestamp, true);
    for (int i = 0; i < CIRCULAR_BUFFER_LENGTH - 1; i++) {
        item.timestamp = 1 + i;
        item.value = 0x1234 + i;
        cb_append(&cb, &item, sizeof(item));
    }

    // Reads values from flash memory and compares them
    const uint32_t MAGIC_HEADER;
    test_create_item_t *flash = (test_create_item_t *)(XIP_BASE + FLASH_BASE + sizeof(MAGIC_HEADER));
    for (int i = 0; i < CIRCULAR_BUFFER_LENGTH - 1; i++) {
        assert(flash[i].timestamp == (uint64_t)(1 + i));
        assert(flash[i].value == 0x1234 + i);
    }

    cleanup();
}

void test_append(void) {
    printf("append ...................");

    test_append1();
    test_append9();
    test_append_circular();
    test_persistent_value();
    printf("ok\n");
}
