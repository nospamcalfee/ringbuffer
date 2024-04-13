/*
 *  FIXME
 */
#include "tests.h"


#define CIRCULAR_BUFFER_LENGTH  512   // (8192 / sizeof(test_item_t))

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


void test_create(void) {
    // FIXME

}
