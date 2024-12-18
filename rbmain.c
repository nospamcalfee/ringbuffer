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
//need a pagebuff to do a write/delete but it is not needed between calls, everyone can use it.
static uint8_t pagebuff[FLASH_PAGE_SIZE];

static rb_errors_t writer(rb_t *rb, uint8_t *data, uint32_t size) {
    uint64_t timestamp = time_us_64();
    float temperature = read_onboard_temperature(TEMPERATURE_UNITS);
    cb_entry_t entry;
    entry.timestamp = timestamp;
    entry.data = temperature;
    printf("Writing timestamp=%.1f,temperature=%.2f size=0x%lx ", (double)entry.timestamp / 1000000, entry.data, size);
    rb_errors_t err = rb_append(rb, 0x7, data, size, pagebuff, true);
    printf(" @0x%lx stat=%d\n", rb->last_wrote, err);
    // hexdump(stdout, rb, 16, 16, 8);
    if (err != RB_OK) {
        printf("some write failure %d\n", err);
    }
    return err;
}

rb_errors_t reader(rb_t *rb, uint8_t *data, uint32_t size) {
    int err;
    uint32_t oldnext = rb->next;
    err = rb_read(rb, 7, data, size);
    if (err >= 0) {
        printf("Just read from 0x%lx to 0x%lx stat=%d size=0x%x\n", oldnext, rb->next, -err, err);
        hexdump(stdout, data, MIN(size, 8), 16, 8);
        return RB_OK; //return after 1 read
    } else {
        printf("some read failure, stop reading %d\n", -err);
    }
    return -err;
}
//allocate first page as ssid storage, rest for test data
// #define SSID_BUFF (__PERSISTENT_TABLE)
// #define SSID_LEN FLASH_SECTOR_SIZE
// #define TEST_BUFF (__PERSISTENT_TABLE + FLASH_SECTOR_SIZE)
// #define TEST_LEN (__PERSISTENT_LEN - SSID_LEN)
//test sharing just one buffer
#define SSID_BUFF (__PERSISTENT_TABLE)
#define SSID_LEN __PERSISTENT_LEN
#define TEST_BUFF (__PERSISTENT_TABLE)
#define TEST_LEN (__PERSISTENT_LEN)

#define SSID_ID 0x3a
//ssid save and recover tests
const char *test_strings[] = {
    "First entry",
    "Second Long entry",
    "Third",
    "A",
    "B",
    "C",
    "D",
    "E",
    "F",
};
#define SSID_TEST_WRITES 7
static rb_t slow_rb; //keep the buffer off the stack
static rb_t ssid_rb; //keep the buffer off the stack
// #define TEST_SIZE (4096-4-4) same as RB_MAX_APPEND_SIZE
// this one will fail writes
// #define TEST_SIZE 8000
#define TEST_SIZE (1)
// #define TEST_SIZE (190)
// #define TEST_SIZE (1024-7)
// #define TEST_SIZE RB_MAX_APPEND_SIZE
// #define TEST_SIZE (1024*3-7)

static uint8_t workdata[TEST_SIZE]; //local data for transferring
/*
 read all ssids from flash. return number of successful reads or negative error
 status
*/
static int read_ssids(rb_t *rb){
    int err;

    err = rb_recreate(rb, SSID_BUFF, SSID_LEN / FLASH_SECTOR_SIZE, CREATE_FAIL);
    if (!(err == RB_OK || err == RB_BLANK_HDR || err == RB_HDR_LOOP)) {
        printf("reopening flash error %d, quitting\n", err);
        exit(1);
    }
    uint32_t loopcount = 0;
    while (true) {
        err = rb_read(rb, SSID_ID, pagebuff, sizeof(pagebuff));

        printf("Reading ssid %ld starting at 0x%lx stat=%d\n\"%s\"\n", loopcount, rb->next, err, pagebuff);
        // hexdump(stdout, pagebuff, err + 1, 16, 8);
        if (err <= 0) {
            printf("some read failure %d\n", err);
            break;
        }
        loopcount++; //count successes
    }
    if (loopcount) {
        return loopcount;
    }
    return -err;
}
//if ssids are not in flash, write them
//for safety write both the ssid and the password as 2 strings to flash
static int wrcnt;
void create_ssid_rb(rb_t *rb, enum init_choices ssid_choice) {
    // create another write/read set of control buffers for an alternative flash buffer containing strings
    int err = rb_recreate(rb, SSID_BUFF, SSID_LEN / FLASH_SECTOR_SIZE, ssid_choice);
    if (!(err == RB_OK || err == RB_BLANK_HDR || err == RB_HDR_LOOP)) {
        printf("starting flash error %d, quitting\n", err);
        exit(1);
    }
}
static rb_errors_t write_ssids(rb_t *rb) {
    //first read existing ssids, see if all full for 3 wifi groups
    //that means read 6 strings, which are ssid, then password
    char tempssid[64]; //note this comes on a very small cpu stack of 4k - ok for a test.
    uint32_t good_reads = SSID_TEST_WRITES - 1; //read_ssids(rb);

    //less than required number of strings, write more.
    for (uint32_t i = good_reads; i < SSID_TEST_WRITES; i++) {
        //finally we get to write some strings
        //build 2 strings for one flash write

        strcpy(tempssid, test_strings[(i + wrcnt) % ARRAY_LENGTH(test_strings)]);
        strcat(tempssid, "\n");
        strcat(tempssid, test_strings[((i + wrcnt) + 1) % ARRAY_LENGTH(test_strings)]);
        strcat(tempssid, "\n");
        wrcnt++;
        rb_errors_t err = rb_append(rb, SSID_ID, tempssid, strlen(tempssid) + 1,
                                    pagebuff, true);
        printf("Just wrote ssid %ld at 0x%lx stat=%d\n%s\n", i, rb->last_wrote, err, tempssid);
        // hexdump(stdout, tempssid, strlen(tempssid) + 1, 16, 8);
        if (err != RB_OK) {
            // printf("some write failure %d\n", err);
            if (err == RB_HDR_LOOP) {
                //not enough room, let caller handle it
                printf("not enough room err=%d, let caller handle it\n", err);
            } else {
                printf("some bad write error=%d\n",err);
            }
            return err;
        }
    }
    //and write another, with a different id for read/write tests
    tempssid[0] = '\x61';
    rb_errors_t terr = rb_append(rb, SSID_ID + 7, tempssid, strlen(tempssid) + 1,
                                pagebuff, true);
    printf("finally wrote ssid 0x%x at 0x%lx stat=%d\n%s\n", SSID_ID + 7, rb->last_wrote, terr, tempssid);

    return terr;
}

int main(void) {
    int loopcount = 0;
    stdio_init_all();
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);

    rb_errors_t err = rb_recreate(&slow_rb, TEST_BUFF, TEST_LEN / FLASH_SECTOR_SIZE,
                               CREATE_INIT_IF_FAIL);
    if (!(err == RB_OK || err == RB_BLANK_HDR || err == RB_HDR_LOOP)) {
        printf("starting flash error %d, quitting\n", err);
        exit(1);
    }
    create_ssid_rb(&ssid_rb, CREATE_FAIL);

    sleep_ms(4000);
    printf("linker defined persistent area 0x%lx, len 0x%lx st=%d\n", __PERSISTENT_TABLE, __PERSISTENT_LEN, err);
    sleep_ms(1000);
    if (write_ssids(&ssid_rb) != RB_OK) {
        //writes that fail are bad, reinit the flash
        create_ssid_rb(&ssid_rb, CREATE_INIT_ALWAYS);
    }
    read_ssids(&ssid_rb);
    for (uint32_t i = 0; i < ARRAY_LENGTH(test_strings); i++) {
        err = rb_delete(&ssid_rb, SSID_ID, (const uint8_t *)test_strings[i], strlen(test_strings[i]), pagebuff);
        if (err == RB_OK) {
            break;
        }
    }
    read_ssids(&ssid_rb);

    while (true) {
        for (uint32_t i = 0; i < sizeof(workdata); i++) {
            workdata[i] = (uint8_t) i;
        }
        err = writer(&slow_rb, workdata, TEST_SIZE);
        if (err != RB_OK && loopcount++ > 60) {
            loopcount = 0;
            printf("flash error %d, reiniting rolling over to first sector\n", err);
            rb_errors_t err = rb_recreate(&slow_rb, TEST_BUFF, TEST_LEN / FLASH_SECTOR_SIZE,
                           CREATE_INIT_ALWAYS);
             if (err != RB_OK) {
                //init failed, bail
                printf("flash error %d, quitting\n", err);
                exit(2);
            }
        }
        err = reader(&slow_rb, workdata, TEST_SIZE);
        if (err != RB_OK) slow_rb.next = 0;
        int ssid_stat = write_ssids(&ssid_rb);
        if (ssid_stat != RB_OK) {
            //writes that fail are bad, reinit the flash
            // create_ssid_rb(&ssid_rb, CREATE_INIT_ALWAYS);
            printf("write ssid failure = %d\n", ssid_stat);
        }
        sleep_ms(1000);
    }
}
