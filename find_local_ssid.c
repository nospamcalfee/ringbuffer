#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "flash_io.h"
#include "find_local_ssid.h"

struct cdll knownnodes; //global list base

#define cast_cdll_to_my_params(pt) (cast_p_to_outer( \
            struct cdll *, pt, \
            struct my_params, ll))

//return 0 if not unique
static int unique_ssid(const uint8_t *ssid) {
    struct cdll *ll;
    struct my_params *test;
    struct my_scan_result *result;
    if (cdll_empty(&knownnodes)) {
        return 1; //if list is empty, it is unique
    }
    cdll_for_each(ll, &knownnodes) {
        test = cast_cdll_to_my_params(ll);
        result = &test->res;
        if (strlen(result->ssid) == strlen(ssid)) {
            int r = memcmp(ssid, result->ssid, strlen(result->ssid));
            if (!r) {
                return r; //found a match, not unique
            }
        }
    }
    return 1; //searched all, not unique
}
//return 1 if new ssid is better than current
static struct my_scan_result *better_rssi(const uint8_t *ssid, int rssi)
{
    struct cdll *ll;
    struct my_params *test;
    struct my_scan_result *result;
    cdll_for_each(ll, &knownnodes) {
        test = cast_cdll_to_my_params(ll);
        result = &test->res;
        if (strlen(result->ssid) == strlen(ssid)) {
            int r = rssi > result->rssi;
            if (r) {
                return result; //found a better rssi
            }
        }
    }
    return NULL; //searched all, no better rssi
}
static int scan_all_result(void *env, const cyw43_ev_scan_result_t *result) {
    if (result) {
        int uniq = 0;
        printf("ssid: %-32s rssi: %4d chan: %3d mac: %02x:%02x:%02x:%02x:%02x:%02x sec: %u\n",
            result->ssid, result->rssi, result->channel,
            result->bssid[0], result->bssid[1], result->bssid[2], result->bssid[3], result->bssid[4], result->bssid[5],
            result->auth_mode);
        if (result->rssi < LOCAL_SCAN_MIN_RSSI || strlen(result->ssid) == 0) {
            printf("scan AP too weak %d or anon=%s\n", result->rssi, result->ssid);
            return 0;
        }

        if (unique_ssid(result->ssid)) {
            uniq = 1;
        }
        //unique or not and better rssi gets a new entry
        if (uniq) {
            struct my_params *listtest = calloc(1, sizeof(*listtest));
            cdll_init(&listtest->ll);
            memcpy(listtest->res.ssid, result->ssid, sizeof(listtest->res.ssid)); //make a copy of the returned struct
            listtest->res.channel = result->channel;
            listtest->res.rssi = result->rssi;
            cdll_insert_node_tail(&listtest->ll, &knownnodes);
            printf("scanlist %p ll=%p\n",  listtest, &listtest->ll);
        } else {
            //not unique, is it a better choice with a better rssi?
            struct my_scan_result *better = better_rssi(result->ssid, result->rssi);
            if (better) {
                better->channel = result->channel;
                better->rssi = result->rssi;
                printf("new better scan %p chan: %3D rssi %4d\n",  better->channel, better->rssi);
            }
        }
    }
    return 0;
}
static void printlist(struct cdll *p)
{
    struct cdll *ll;
    struct my_params *test;
    struct my_scan_result *result;
    cdll_for_each(ll, p) {
        test = cast_cdll_to_my_params(ll);
        result = &test->res;
        printf("printlist ssid: %-32s rssi: %4d chan: %3d\n",
            result->ssid, result->rssi, result->channel);
        // printf("printlist %p ll=%p\n", test, ll);
    }

}
/* remove a cdll list included in a my_params struct */
void removelist(struct cdll *p)
{
    struct cdll *vlist;
    struct my_params *test;
    //cannot use cdll_for_each because loop uses freed pointer
    struct cdll *next_vlist = p;
    for (vlist = p->next; vlist != p; vlist = next_vlist) {
        test = cast_cdll_to_my_params(vlist);
        next_vlist = vlist->next;
        printf("fifo delete %p ll=%p known=%p\n", vlist, next_vlist, p->next);
        cdll_delete_node(vlist);
        free(cast_cdll_to_my_params(vlist));
    }
}

/*
    return 0 if all ssids have been scanned, or failure code
*/
int scan_find_all_ssids(void)
{
    cdll_init(&knownnodes); //init parent
    cyw43_arch_deinit();

    if (cyw43_arch_init()) {
        printf("failed to initialise\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();

    struct my_params params = { .found = false, .ll = &knownnodes};
    cyw43_wifi_scan_options_t scan_options = {0};
    printf("\nPerforming wifi scan and list\n");
    int err = cyw43_wifi_scan(&cyw43_state, &scan_options, &params, scan_all_result);
    assert(err == 0);

    while (cyw43_wifi_scan_active(&cyw43_state)) {
        // cyw43_arch_poll();
        // cyw43_arch_wait_for_work_until(make_timeout_time_ms(1000));
        sleep_ms(1000);
    }
    printlist(&knownnodes);
    // removelist(&knownnodes); must be removed or will leak....
    return 0;
}

// scan list, find most powerful AP
// return password string AND my_scan_result ptr or NULL
struct my_scan_result *scan_find_best_ap(char *password){
    struct cdll *ll;
    struct my_params *test;
    struct my_params *best = NULL; //best ap found
    password[0] = '\0';

    cdll_for_each(ll, &knownnodes) {
        test = cast_cdll_to_my_params(ll);
        if (test->found == 0) {
            //ignore nodes already checked
            if (best == NULL) {
                best = test; //first one is best so far
            }
            if (test->res.rssi > best->res.rssi) {
                best = test; //found a new best
            }
            rb_errors_t  err = flash_io_find_matching_ssid(best->res.ssid, password);
            if (err < 0) {
                best= NULL;
            } else {
                break; //found best and a flash match
            }
        }
    }
    if (best != NULL) {
        best->found = 42; //set flag in linked list as used
        return &best->res;
    }
    return NULL;    //no AP found
}