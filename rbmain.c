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


uint64_t extract_timestamp(void *entry) {
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

static rb_errors_t task_measure(rb_t *rb) {
    uint64_t timestamp = time_us_64();
    float temperature = read_onboard_temperature(TEMPERATURE_UNITS);
    cb_entry_t entry;
    entry.timestamp = timestamp;
    entry.data = temperature;
    printf("writing timestamp=%.1f,temperature=%.2f\n", (double)entry.timestamp / 1000000, entry.data);
    return rb_append(rb, 0x7, &entry, sizeof(entry));
}

static void task_report(rb_t *rb) {
    (void) rb;
    // bool button = bb_get_bootsel_button();
    // rb_entry_t entry;
    // rb_cursor_t cursor;

    // if (button) {  // Push BOOTSEL button
    //     printf("--------DESCENDING TIME SERIES\n");
    //     cb_open_cursor(cb, &cursor, RB_CURSOR_DESCENDING);
    //     while (cb_get_next(&cursor, &entry)) {
    //         printf("timestamp=%.1f,temperature=%.2f\n", (double)entry.timestamp / 1000000, entry.data);
    //     }
    // } else { // Release BOOTSEL button
    //     printf("--------ASCENDING TIME SERIES\n");
    //     cb_open_cursor(cb, &cursor, RB_CURSOR_ASCENDING);

    //     while (cb_get_next(&cursor, &entry)) {
    //         printf("timestamp=%.1f,temperature=%.2f\n", (double)entry.timestamp / 1000000, entry.data);
    //     }
    // }
    return;
}
static rb_t rb; //keep the buffer off the stack

int main(void) {

    stdio_init_all();
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);

    rb_errors_t err = rb_create(&rb, __PERSISTENT_TABLE, 1, false);
    if (!(err == RB_OK || err == RB_BLANK_HDR)) {
        printf("starting flash error %d, reiniting\n", err);
        err = rb_create(&rb, __PERSISTENT_TABLE, 1, true); //start over
        if (err != RB_OK) {
            //init failed, bail
            printf("starting flash error %d, quitting\n", err);
            exit(0);
        }
    }
    printf("linker defined persistent area, len 0x%lx 0x%lx, st=%d\n", __PERSISTENT_TABLE, __PERSISTENT_LEN, err);
    while (true) {
        err = task_measure(&rb);
        if (err != RB_OK) {
            printf("flash error %d, reiniting rolling over to first sector\n", err);
            err = rb_create(&rb, __PERSISTENT_TABLE, 1, true); //start over
            if (err != RB_OK) {
                //init failed, bail
                printf("flash error %d, quitting\n", err);
                exit(0);
            }
            //exit(0); //stop after writing full buffers
        }
        task_report(&rb);
        sleep_ms(1000);
    }
}
