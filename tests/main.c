/*
 * Copyright 2024, Hiroyuki OYAMA. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "tests.h"

int main(void) {
    stdio_init_all();
    printf("Start all tests\n");

    test_create();
    test_append();
    test_cursor();
    test_restore();
    // test_across_flash_sectors();
    printf("All tests are ok\n");
}
