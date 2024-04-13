/*
 * Copyright 2024, Hiroyuki OYAMA. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _TESTS_H_
#define _TESTS_H_

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pico/stdlib.h>
#include <hardware/adc.h>
#include "circular_buffer.h"

extern const uint32_t FLASH_BASE;

void test_create(void);
void test_append(void);
void test_cursor(void);
void test_restore(void);
//void test_across_flash_sectors();
#endif
