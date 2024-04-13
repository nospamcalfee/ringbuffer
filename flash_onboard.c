/*
 * Copyright 2024, Hiroyuki OYAMA. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <string.h>
#include <hardware/flash.h>
#include <hardware/sync.h>
#include <hardware/regs/addressmap.h>
#include "flash.h"


const uint32_t FLASH_BASE = 0x1F0000;

int flash_read(uint32_t address, void *buffer, size_t size) {
    uint8_t* p = (uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + address);
    memcpy(buffer, p, size);
    return 0;
}

int flash_prog(uint32_t address, const void *buffer, size_t size) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(address, buffer, size);
    restore_interrupts(ints);
    return 0; // Success
}

int flash_erase(uint32_t address, size_t size) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(address, size);
    restore_interrupts(ints);
    return 0; // Success
}
