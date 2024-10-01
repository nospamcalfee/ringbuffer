/*
 * Copyright 2024, Steve Calfee. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <pico/stdlib.h>
#include <hardware/adc.h>
#include "ring_buffer.h"
#include "bootsel_button.h"
#include <inttypes.h>


#define TEMPERATURE_UNITS 'C'

int hexdump(FILE *fd, void const *ptr, size_t length, int linelen, int split);

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
//need a pagebuff per writer, but only one writer per flash allocation
static uint8_t pagebuff[FLASH_PAGE_SIZE];
static rb_errors_t writer(rb_t *rb, uint8_t *data, uint32_t size) {
    uint64_t timestamp = time_us_64();
    float temperature = read_onboard_temperature(TEMPERATURE_UNITS);
    cb_entry_t entry;
    entry.timestamp = timestamp;
    entry.data = temperature;
    printf("writing timestamp=%.1f,temperature=%.2f size=0x%lx ", (double)entry.timestamp / 1000000, entry.data, size);
    rb_errors_t err = rb_append(rb, 0x7, data, size, pagebuff, true);
    printf("Just wrote at  0x%lx stat=%d\n", rb->next, err);
    // hexdump(stdout, rb, 16, 16, 8);
    if (err != RB_OK) {
        printf("some write failure %d\n", err);
    }
    return err;
}

rb_errors_t reader(rb_t *rb, uint8_t *data, uint32_t size) {
    rb_errors_t err = RB_OK;
    do {
        uint32_t oldnext = rb->next;
        int res = rb_read(rb, 7, data, size);
        if (res < 0) err = -res;
        printf("Just read from 0x%lx to 0x%lx stat=%d size=%d\n", oldnext, rb->next, err, res);
        if (err == RB_OK) {
            hexdump(stdout, data, MIN(size, 8), 16, 8);
            return err; //return after 1 read
        } else {
            printf("some read failure, stop reading %d\n", err);
        }
    } while (err == RB_OK);
    return err;
}
static rb_t write_rb; //keep the buffer off the stack
static rb_t read_rb; //keep the buffer off the stack
static uint8_t workdata[4096]; //local data for transferring
// #define TEST_SIZE (4096-4-4)
// #define TEST_SIZE (1)
// #define TEST_SIZE (190)
// #define TEST_SIZE (1024-7)
#define TEST_SIZE (1024*3-7)
int main(void) {
    int loopcount = 0;
    stdio_init_all();
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);

    rb_errors_t err = rb_create(&write_rb, __PERSISTENT_TABLE, MAX_SECTS, false, true);
    if (!(err == RB_OK || err == RB_BLANK_HDR || err == RB_HDR_LOOP)) {
        printf("starting flash error %d, reiniting\n", err);
        err = rb_create(&write_rb, __PERSISTENT_TABLE, MAX_SECTS, true, true); //start over
        if (err != RB_OK) {
            //init failed, bail
            printf("starting flash error %d, quitting\n", err);
            exit(0);
        }
    }
    printf("linker defined persistent area 0x%lx, len 0x%lx st=%d\n", __PERSISTENT_TABLE, __PERSISTENT_LEN, err);
    while (true) {
        for (uint32_t i = 0; i < sizeof(workdata); i++) {
            workdata[i] = (uint8_t) i;
        }
        err = writer(&write_rb, workdata, TEST_SIZE);
        if (err != RB_OK && loopcount++ > 60) {
            loopcount = 0;
            printf("flash error %d, reiniting rolling over to first sector\n", err);
            // err = rb_create(&write_rb, __PERSISTENT_TABLE, MAX_SECTS, true, true); //start over
            if (err != RB_OK) {
                //init failed, bail
                printf("flash error %d, quitting\n", err);
                exit(0);
            }
            //exit(0); //stop after writing full buffers
        }
        if (read_rb.number_of_bytes == 0) {
            //first time through, or write reinited, init the read buffer
            err = rb_create(&read_rb, __PERSISTENT_TABLE, MAX_SECTS, false, false); //start over
            if (err != RB_OK && err != RB_BLANK_HDR) {
                //some failure, maybe need to wait for writer task?
                printf("some read init failure %d, quitting\n", err);
                exit(0);
            }
        } else {
            err = reader(&read_rb, workdata, TEST_SIZE);
            if (err != RB_OK) read_rb.next = 0;         
        }
        sleep_ms(1000);
    }
}
