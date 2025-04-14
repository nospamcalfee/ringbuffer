#include "ring_buffer.h"
#include "pico/stdlib.h"
#include "flash_io.h"

#define SSID_BUFF (__PERSISTENT_TABLE)
#define SSID_LEN __PERSISTENT_LEN
#define NAME_BUFF (__PERSISTENT_TABLE)
#define NAME_LEN __PERSISTENT_LEN
#define SSID_ID 0x01
#define HOSTNAME_ID 0x02
/*
 need a pagebuff to do a write/delete but it is not needed between calls,
 everyone can use it. for this api, assumes all i/o will be smaller than
 FLASH_PAGE_SIZE
*/
uint8_t pagebuff[FLASH_PAGE_SIZE];

/*
 read all idx from flash. return number of successful reads or negative error
 status
*/
int read_flash_ids(int id, uint32_t flash_buf, uint32_t flash_len){
    int err;
    rb_t rb;

    err = rb_recreate(&rb, flash_buf, flash_len / FLASH_SECTOR_SIZE, CREATE_INIT_IF_FAIL);
    if (!(err == RB_OK)) {
        printf("reopening read_flash_ids flash error %d, quitting\n", err);
        return err;
    }
    uint32_t loopcount = 0;
    while (true) {
        err = rb_read(&rb, id, pagebuff, sizeof(pagebuff));

        // hexdump(stdout, pagebuff, err + 1, 16, 8);
        if (err <= 0) {
            if (err != RB_BLANK_HDR) {
                printf("some non-blank read failure %d\n", err);
            }
            return loopcount; //return number found, normal exit
        } else {
            printf("Reading flash id=%d %ld starting at 0x%lx stat=%d\n\"%s\"\n", id, loopcount, rb.next, err, pagebuff);
        }
        loopcount++; //count successes
    }
    // if (loopcount) {
    //     return loopcount;
    // }
    return err;
}
//read a specific flash entry entry n
rb_errors_t read_flash_id_n(int id, uint32_t flash_buf, uint32_t flash_len, int n){
    int i;
    int err;
    rb_t rb;

    err = rb_recreate(&rb, flash_buf, flash_len / FLASH_SECTOR_SIZE, CREATE_INIT_IF_FAIL);
    if (!(err == RB_OK)) {
        printf("reopening read_flash_id_n flash error %d, quitting\n", err);
        return err;
    }
    for (i = 1; i < n; i++) {
        err = rb_read(&rb, id, pagebuff, sizeof(pagebuff));

        printf("skipping flash entry %d starting at 0x%lx stat=%d\n\"%s\"\n", i, rb.next, err, pagebuff);
        if (err <= 0) {
            printf("some read failure %d\n", err);
            return err;
        }
    }
    //now read the desired flash entry
    err = rb_read(&rb, id, pagebuff, sizeof(pagebuff));

    printf("reading flash entry %d starting at 0x%lx stat=%d\n\"%s\"\n", i, rb.next, err, pagebuff);
    if (err <= 0) {
        printf("final %d read failure %d\n", i, err);
        return err;
    }
    return err; //return actual length
}
//read a specific flash entry entry n
rb_errors_t read_flash_id_latest(int id, uint32_t flash_buf, uint32_t flash_len){
    int err;
    rb_t rb;

    err = read_flash_ids(id, flash_buf, flash_len);
    if (err <= 0) {
        printf("final read failure %d\n", err);
        return err;
    }
    return read_flash_id_n(id, flash_buf, flash_len, err);
}

rb_errors_t flash_io_write_flash_id(int id, uint32_t flash_buf, uint32_t flash_len, uint8_t *buff, uint32_t blen) {
    int i;
    int err;
    rb_t trb;
    //first read last entry already in flash
    err = read_flash_id_latest(id, flash_buf, flash_len);
    if (err == blen) {
        //we got something, same length, check if same as the new
        err = memcmp(pagebuff, buff, blen);
        if (err == 0) {
            //exact same data, so do not write the new data
            printf("no need to write, data is duplicated\n");
            return 0;
        }
    }
    err = rb_recreate(&trb, flash_buf, flash_len / FLASH_SECTOR_SIZE, CREATE_INIT_IF_FAIL);
    if (!(err == RB_OK || err == RB_BLANK_HDR)) {
        printf("write reopening flash error flash_io_write_flash_id %d, quitting\n", err);
        return err;
    }
    rb_errors_t terr = rb_append(&trb, id, buff, blen, pagebuff, true);
    printf("finally wrote flash id=0x%x at 0x%lx stat=%d len=%d",
            id, trb.last_wrote, terr, blen);
    for (i = 0; i < blen; i++) {
        if (i % 8 == 0) {
            printf("\n");
        }
        printf(" %02x", buff[i]);
    }
    if (i / 8 % blen) {
        printf("\n");
    }
    return blen;
}

rb_errors_t flash_io_erase_ssids_hostnames() {
    rb_t trb;
    //dangerous routine to erase all the ssid and hostname flash to reinit for user
    rb_errors_t err = rb_recreate(&trb, SSID_BUFF, SSID_LEN / FLASH_SECTOR_SIZE, CREATE_INIT_ALWAYS);
    return err;
}
//man, maintaining a clean flash is tough. Remove ssids with replaced passwords from flash
// also remove any null length ssid
rb_errors_t flash_io_erase_redundant_ssids(char *ss) {
    rb_t rb;
    int sslen = strlen(ss) + 1; //include \0 in string check
    rb_errors_t terr = rb_recreate(&rb, SSID_BUFF, SSID_LEN / FLASH_SECTOR_SIZE, CREATE_FAIL);
    if (!(terr == RB_OK || terr == RB_BLANK_HDR)) {
        printf("reopening flash_io_erase_redundant_ssids flash error %d, quitting\n", terr);
        return terr; //should never happen
    }
    while (terr >= 0 && sslen) {
        terr = rb_find(&rb, SSID_ID, ss, sslen, pagebuff); //find first copy of ssid
        if (terr < 0) {
            //some error
            printf("some flash_io_erase_redundant_ssids find failure %d looking for \"%s\"\n", terr, ss);
            break; //exit loop on failure
        } else {
            // at least one ssid matches, see if it is a duplicate because it is older
            //rb.next is set to after the found entry, so start find from there
            rb_errors_t next_ssid = rb_find(&rb, SSID_ID, ss, sslen, pagebuff);
            if (next_ssid >=0) {
                //found another copy of ssid, so erase earlier one
                rb.next = terr; //use returned find offset from first match
                printf("removing redundant @ %d, newer @ %d\n", terr, next_ssid);
                rb_delete(&rb, SSID_ID, ss, sslen, pagebuff); //so delete redundant
            } else {
                //could not find second entry, either a true error or no more
                //redundant entries exist.
                printf("second find failed redundant @ %d, newer @ %d\n", terr, next_ssid);
                if (!(next_ssid == RB_BLANK_HDR || next_ssid == RB_HDR_ID_NOT_FOUND)) {
                    return next_ssid; //so exit with n error
                } else {
                    return RB_OK;
                }
            }
        }
    }
    return terr;
}
//find matching ssid in flash.
//return negative error or 0 ok, and return password for my entry data
rb_errors_t flash_io_find_matching_ssid(char *ss, char *pw) {
    rb_t rb;
    rb_errors_t terr;
    int sslen = strlen(ss);
    //make sure only one ssid for this ss exists in flash
     terr = flash_io_erase_redundant_ssids(ss);
    if (terr < 0) {
        printf("finding flash error %d, quitting\n", terr);
        return terr;
    }

    terr = rb_recreate(&rb, SSID_BUFF, SSID_LEN / FLASH_SECTOR_SIZE, CREATE_FAIL);
    if (terr != RB_OK && terr != RB_BLANK_HDR) {
        printf("reopening finding flash error %d, quitting\n", terr);
        exit(1); //should never happen
    }

    while (terr >= 0 && sslen) {
        terr = rb_find(&rb, SSID_ID, ss, sslen, pagebuff);
        if (terr < 0) {
            //some error
            printf("some find failure %d looking for \"%s\"\n", terr, ss);
            break; //exit loop on failure
        } else {
            //we found a matching ssid, return its password, first read flash
            rb.next = terr; //use returned find offset
            rb_read(&rb, SSID_ID, pagebuff, sizeof(pagebuff));
            int ssidlen = strlen(pagebuff);
            printf("find AP found %s pw %s\n", pagebuff, pagebuff + ssidlen + 1);
            if (sslen == ssidlen) {
                int pwlen = strlen(pagebuff + ssidlen + 1);
                //copy buffered password and its \0 terminator
                memcpy(pw, pagebuff + ssidlen + 1, pwlen + 1);
                break;
            }
        }
    }
    return terr;
}

//for safety write both the ssid and the password as 2 strings to flash
//write a new ssid/pw pair
rb_errors_t flash_io_write_ssid(char * ss, char *pw) {
    char tempssid[64]; //note this comes on a very small cpu stack of 4k

    //build 2 strings for one flash write
    //fixme buffer overflow is possible, limit strings
    int s1len = strlen(ss) + 1;
    memcpy(tempssid, ss, s1len);
    int s2len = strlen(pw) + 1;
    memcpy(tempssid + s1len, pw, s2len);

    rb_errors_t terr =  flash_io_write_flash_id(SSID_ID, SSID_BUFF, SSID_LEN, tempssid, s1len+s2len);
    if (terr > 0) {
        flash_io_erase_redundant_ssids(ss); // erase matching ssids, not ssid/pw combos
    }
    return terr;
}

rb_errors_t flash_io_read_latest_hostname(void) {
        return read_flash_id_latest(HOSTNAME_ID, NAME_BUFF, NAME_LEN);
}

rb_errors_t flash_io_write_hostname(char *hostname, uint32_t nlen) {
    rb_errors_t terr =  flash_io_write_flash_id(HOSTNAME_ID, NAME_BUFF, NAME_LEN, hostname, nlen);
    printf("finally wrote hostname id=0x%x stat=%d name=%s\n",
            HOSTNAME_ID, terr, hostname);
    if (terr > 0) {
        flash_io_erase_redundant_ssids(hostname);
    }
    return terr;
}
