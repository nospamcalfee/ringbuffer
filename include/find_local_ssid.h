/*
 home/calfee/Projects/picow_webapp/include/find_local_ssid.h
 */
#ifndef FIND_LOCAL_SSID_H
#define FIND_LOCAL_SSID_H

#include "cdll.h"
#include "post_handler.h"

extern struct cdll knownnodes;

struct my_scan_result {
    uint8_t ssid[LWIP_POST_BUFSIZE];   ///< wlan access point name
    uint16_t channel;   ///< wifi channel
    int16_t rssi;       ///< signal strength
};

struct my_params{
    struct cdll ll;
    int found;
    struct my_scan_result res;
};

#define LOCAL_SCAN_MIN_RSSI (-80)
/*
    return 0 if all ssids have been scanned, or failure code
*/
int scan_find_all_ssids();

/* remove a cdll list included in a my_params struct */
void removelist(struct cdll *p);

// scan list, find most powerful ap, return password from flash
struct my_scan_result *scan_find_best_ap(char *password);

#endif //FIND_LOCAL_SSID_H