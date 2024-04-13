/*
 * Copyright 2024, Hiroyuki OYAMA. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <pico/stdlib.h>
#include <hardware/adc.h>
#include "circular_buffer.h"
#include "bootsel_button.h"
#include <inttypes.h>


#define TEMPERATURE_UNITS 'C'
#define CIRCULAR_BUFFER_LENGTH  20

extern const uint32_t FLASH_BASE;

typedef struct {
    uint64_t timestamp;
    float data;
} cb_entry_t;


static uint64_t extract_timestamp(void *entry) {
    cb_entry_t *cb_entry = (cb_entry_t *)entry;
    return cb_entry->timestamp;
}

float read_onboard_temperature(const char unit) {
    /* 12-bit conversion, assume max value == ADC_VREF == 3.3 V */
    const float conversionFactor = 3.3f / (1 << 12);

    float adc = (float)adc_read() * conversionFactor;
    float tempC = 27.0f - (adc - 0.706f) / 0.001721f;

    if (unit == 'C') {
        return tempC;
    } else if (unit == 'F') {
        return tempC * 9 / 5 + 32;
    }

    return -1.0f;
}

static void task_measure(cb_t *cb) {
    uint64_t timestamp = time_us_64();
    float temperature = read_onboard_temperature(TEMPERATURE_UNITS);
    cb_entry_t entry;
    entry.timestamp = timestamp;
    entry.data = temperature;
    cb_append(cb, &entry, sizeof(entry));
}

static void task_report(cb_t *cb) {
    bool button = bb_get_bootsel_button();
    cb_entry_t entry;
    cb_cursor_t cursor;

    if (button) {  // Push BOOTSEL button
        printf("--------DESCENDING TIME SERIES\n");
        cb_open_cursor(cb, &cursor, RB_CURSOR_DESCENDING);
        while (cb_get_next(&cursor, &entry)) {
            printf("timestamp=%.1f,temperature=%.2f\n", (double)entry.timestamp / 1000000, entry.data);
        }
    } else { // Release BOOTSEL button
        printf("--------ASCENDING TIME SERIES\n");
        cb_open_cursor(cb, &cursor, RB_CURSOR_ASCENDING);

        while (cb_get_next(&cursor, &entry)) {
            printf("timestamp=%.1f,temperature=%.2f\n", (double)entry.timestamp / 1000000, entry.data);
        }
    }
}

int main(void) {
    cb_t cb;

    stdio_init_all();
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);

    cb_create(&cb, FLASH_BASE, CIRCULAR_BUFFER_LENGTH, sizeof(cb_entry_t), &extract_timestamp, false);
    while (true) {
        task_measure(&cb);
        task_report(&cb);
        sleep_ms(1000);
    }
}
