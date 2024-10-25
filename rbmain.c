/*
 * Copyright 2024, Steve Calfee. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 * Idea spawned from circular_buffer app
 * Copyright 2024, Hiroyuki OYAMA. All rights reserved.
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
static uint8_t ssidbuf[FLASH_PAGE_SIZE]; //fixme do we need 2 write buffers?
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
//alocate first page as ssid storage, rest for test data
#define SSID_BUFF (__PERSISTENT_TABLE)
#define SSID_LEN FLASH_SECTOR_SIZE
#define TEST_BUFF (__PERSISTENT_TABLE + FLASH_SECTOR_SIZE)
#define TEST_LEN (__PERSISTENT_LEN - SSID_LEN)

#define SSID_ID 0x3a
//ssid save and recover tests
const char * test_strings[] = {
    "First entry",
    "Second Long entry",
    "Third",
};
#define SSID_TEST_WRITES 7
static rb_t write_rb; //keep the buffer off the stack
static rb_t read_rb; //keep the buffer off the stack
static rb_t write_ssid_rb; //keep the buffer off the stack
static rb_t read_ssid_rb; //keep the buffer off the stack
static uint8_t workdata[FLASH_SECTOR_SIZE]; //local data for transferring
//if ssids are not in flash, write them
//for safety write both the ssid and the password as 2 strings to flash
static rb_errors_t write_ssids(rb_t *rb, rb_t *rdrb) {
    //first read existing ssids, see if all full for 3 wifi groups
    //that means read 6 strings, which are ssid, then password
    char tempssid[64]; //note this comes on a very small cpu stack of 4k - ok for a test.
    int res;
    uint32_t good_reads;
    for (good_reads = 0; good_reads < SSID_TEST_WRITES; good_reads++)
    {
        uint32_t oldnext = rdrb->next;
        res = rb_read(rdrb, SSID_ID, tempssid, sizeof(tempssid));
        printf("Just read from 0x%lx to 0x%lx stat=%d size=%d\n", oldnext, rdrb->next, -res, res);
        if (res < 0)  {
            if (-res == RB_HDR_ID_NOT_FOUND) {
                //we only found some of the desired records
                printf("could not find my id after %ld good reads\n", good_reads);
            }
            if (-res != RB_OK) {
                printf("some read failure, stop reading %d\n", -res);
            }
            break; //quit reading on errors
        } else {
            //we just got a string, just print it. //fixme check it.
            printf("%s\n", tempssid);
            hexdump(stdout, tempssid, MIN(res, 8), 16, 8);
        }
    }
    //less than required number of strings, write more.
    for (uint32_t i = good_reads; i < SSID_TEST_WRITES; i++) {
        //finally we get to write some strings
        //build 2 strings for one flash write
        strcpy(tempssid, test_strings[i % ARRAY_LENGTH(test_strings)]);
        strcat(tempssid, "\n");
        strcat(tempssid, test_strings[(i + 1) % ARRAY_LENGTH(test_strings)]);
        strcat(tempssid, "\n");
        rb_errors_t err = rb_append(rb, SSID_ID, tempssid, strlen(tempssid) + 1,
                                    ssidbuf, true);
        printf("Just wrote ssid %ld %s at 0x%lx stat=%d\n", i, tempssid, rb->next, err);
        hexdump(stdout, tempssid, strlen(tempssid) + 1, 16, 8);
        if (err != RB_OK) {
            printf("some write failure %d\n", err);
            if (err == RB_HDR_LOOP) {
                //not enough room, let caller handle it
                printf("not enough room err=%d, let caller handle it\n", err);
                return err;
            }
        }
    }
    return res;
}
//read all ssids from flash
static rb_errors_t read_ssids(rb_t *rb){
    int err;

    err = rb_recreate(rb, SSID_BUFF, SSID_LEN / FLASH_SECTOR_SIZE, CREATE_FAIL, READ_OPEN);
    if (!(err == RB_OK || err == RB_BLANK_HDR || err == RB_HDR_LOOP)) {
        printf("reopening flash error %d, quitting\n", err);
        exit(1);
    }

    for (uint32_t i = 0; i < SSID_TEST_WRITES; i++) {
        err = rb_read(rb, SSID_ID, ssidbuf, sizeof(ssidbuf));

        printf("Reading ssid %ld %s at 0x%lx stat=%d\n", i, ssidbuf, rb->next, err);
        hexdump(stdout, ssidbuf, err + 1, 16, 8);
        if (err <= 0) {
            printf("some read failure %d\n", err);
            return err;
        }
    }
    return err;
}
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

    rb_errors_t err = rb_recreate(&write_rb, TEST_BUFF, TEST_LEN / FLASH_SECTOR_SIZE,
                               CREATE_INIT_IF_FAIL, WRITE_OPEN);
    if (!(err == RB_OK || err == RB_BLANK_HDR || err == RB_HDR_LOOP)) {
        printf("starting flash error %d, quitting\n", err);
        exit(1);
    }
// create another write/read set of control buffers for an alternative flash buffer containing strings
    err = rb_recreate(&write_ssid_rb, SSID_BUFF, SSID_LEN / FLASH_SECTOR_SIZE,
                                  CREATE_INIT_IF_FAIL, WRITE_OPEN);
    if (!(err == RB_OK || err == RB_BLANK_HDR || err == RB_HDR_LOOP)) {
        printf("starting flash error %d, quitting\n", err);
        exit(1);
    }
    err = rb_recreate(&read_ssid_rb, SSID_BUFF, SSID_LEN / FLASH_SECTOR_SIZE,
                                  CREATE_FAIL, READ_OPEN);
    if (!(err == RB_OK || err == RB_BLANK_HDR || err == RB_HDR_LOOP)) {
        printf("starting flash error %d, quitting\n", err);
        exit(1);
    }

    printf("linker defined persistent area 0x%lx, len 0x%lx st=%d\n", __PERSISTENT_TABLE, __PERSISTENT_LEN, err);

    write_ssids(&write_ssid_rb, &read_ssid_rb);
    read_ssids(&read_ssid_rb);

    while (true) {
        for (uint32_t i = 0; i < sizeof(workdata); i++) {
            workdata[i] = (uint8_t) i;
        }
        err = writer(&write_rb, workdata, TEST_SIZE);
        if (err != RB_OK && loopcount++ > 60) {
            loopcount = 0;
            printf("flash error %d, reiniting rolling over to first sector\n", err);
             if (err != RB_OK) {
                //init failed, bail
                printf("flash error %d, quitting\n", err);
                exit(2);
            }
        }
        if (read_rb.number_of_bytes == 0) {
            //first time through, or write reinited, init the read buffer
            err = rb_recreate(&read_rb, TEST_BUFF, TEST_LEN / FLASH_SECTOR_SIZE,
                            CREATE_FAIL, READ_OPEN); //start over
            if (err != RB_OK && err != RB_BLANK_HDR) {
                //some failure, maybe need to wait for writer task?
                printf("some read init failure %d, quitting\n", err);
                exit(3);
            }
        } else {
            err = reader(&read_rb, workdata, TEST_SIZE);
            if (err != RB_OK) read_rb.next = 0;         
        }
        sleep_ms(1000);
    }
}
