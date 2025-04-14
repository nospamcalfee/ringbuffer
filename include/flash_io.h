#ifndef _FLASH_IO_H_
#define _FLASH_IO_H_
#include "ring_buffer.h"

#define SSID_BUFF (__PERSISTENT_TABLE)
#define SSID_LEN __PERSISTENT_LEN
#define SSID_ID 0x01
//need a pagebuff to do a write/delete but it is not needed between calls, everyone can use it.
//for this api, assumes all i/o will be smaller than FLASH_PAGE_SIZE
extern uint8_t pagebuff[FLASH_PAGE_SIZE];

/*
 read all idx from flash. return number of successful reads or negative error
 status
*/
int read_flash_ids(int id, uint32_t flash_buf, uint32_t flash_len);
//read a specific flash entry entry n
int read_flash_id_n(int id, uint32_t flash_buf, uint32_t flash_len, int n);
/*
 read all ssids from flash. return number of successful reads or negative error
 status
*/
//if ssids are not in flash, write them
//for safety write both the ssid and the password as 2 strings to flash
// void create_ssid_rb(rb_t *rb, enum init_choices ssid_choice);
//write a new ssid/pw pair
rb_errors_t flash_io_write_ssid(char * ss, char *pw);
rb_errors_t flash_io_write_hostname(char *hostname, uint32_t nlen);
rb_errors_t flash_io_read_latest_hostname(void);
rb_errors_t flash_io_erase_ssids_hostnames(void);
rb_errors_t flash_io_find_matching_ssid(char *ss, char *pw);
#endif //_FLASH_IO_H_