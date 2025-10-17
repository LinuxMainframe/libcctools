/*
 * file: test_network.c
 * author: Aidan A. Bradley
 * date: 10/08/2025 (mm/dd/yyyy)
 * version: 0.0.2
 * _____________________________ 
 * Standalone test for network module.
 * 
 * This file exercises the NetworkMonitor: creates it, runs background checks,
 * queries state, modifies config, and destroys it. Outputs results to stdout for
 * verification. Run with: gcc test_network.c network.c -o test_net -lpthread
 * Then: ./test_net
 * 
 * Expected: WAN/LAN status prints (e.g., true/false based on your network),
 * timestamps update, no crashes/leaks. Use valgrind for deeper checks.
 */

// Includes
#include "network.h"
#include <stdio.h>    // For printf
#include <stdlib.h>   // For free
#include <unistd.h>   // For sleep
#include <errno.h>    // For errno
#include <string.h>

int main() {
    // Step 1: Create config (custom values for testing)
    NetworkConfig cfg = {
        .timeout_ms = 500,                // Shorter timeout for quick tests
        .check_interval_sec = 2,          // Frequent checks to see updates
        .proxy_url = NULL,                // No proxy for basic test
        .wan_test_host = "8.8.8.8",       // Google DNS
        .wan_test_port = 53,              // DNS port
        .lan_interface = "lo"             // Use loopback for reliable testing
    };

    printf("Creating NetworkMonitor with config: timeout=%dms, interval=%ds, WAN host=%s:%d, LAN iface=%s\n",
           cfg.timeout_ms, cfg.check_interval_sec, cfg.wan_test_host, cfg.wan_test_port, cfg.lan_interface);

    // Step 2: Instantiate monitor
    NetworkMonitorHandle mon = network_monitor_new(&cfg);
    if (!mon) {
        printf("ERROR: Failed to create NetworkMonitor (malloc, thread, or interface error: %s)\n", strerror(errno));
        return 1;
    }
    printf("NetworkMonitor created successfully.\n");

    // Step 3: Initial query (before first check; expect false/0)
    printf("\n--- Initial State ---\n");
    char* initial_info = network_monitor_to_string(mon);
    if (initial_info) {
        printf("%s\n", initial_info);
        free(initial_info);
    } else {
        printf("ERROR: to_string failed\n");
    }
    printf("WAN: %s, LAN: %s, Last Check: %ld, Last Error: %d\n",
           get_wan_status(mon) ? "UP" : "DOWN",
           get_lan_status(mon) ? "UP" : "DOWN",
           get_last_check_time(mon),
           get_last_error(mon));

    // Step 4: Let thread run for checks (3 intervals ~6s)
    printf("\n--- Running checks (waiting ~6s) ---\n");
    for (int i = 0; i < 3; i++) {
        sleep(2);  // Align with interval
        char* info = network_monitor_to_string(mon);
        if (info) {
            printf("Check %d: %s\n", i + 1, info);
            free(info);
        } else {
            printf("Check %d: ERROR: to_string failed\n", i + 1);
        }
        printf("WAN: %s, LAN: %s, Last Check: %ld, Last Error: %d\n",
               get_wan_status(mon) ? "UP" : "DOWN",
               get_lan_status(mon) ? "UP" : "DOWN",
               get_last_check_time(mon),
               get_last_error(mon));
    }

    // Step 5: Test setters (modify live config)
    printf("\n--- Testing Setters (changing timeout to 2000ms, WAN host to 1.1.1.1:443, LAN iface to lo) ---\n");
    set_timeout_ms(mon, 2000);
    set_wan_test_host(mon, "1.1.1.1");
    set_wan_test_port(mon, 443);  // HTTPS port for Cloudflare
    set_lan_interface(mon, "lo");  // Loopback for always-up LAN test
    set_proxy(mon, "http://example-proxy:8080");  // Stub; not used yet
    set_check_interval_sec(mon, 3);

    // Wait one more interval to see updated behavior
    sleep(3);
    char* after_setters_info = network_monitor_to_string(mon);
    if (after_setters_info) {
        printf("After setters: %s\n", after_setters_info);
        free(after_setters_info);
    } else {
        printf("After setters: ERROR: to_string failed\n");
    }
    printf("WAN: %s, LAN: %s, Last Check: %ld, Last Error: %d\n",
           get_wan_status(mon) ? "UP" : "DOWN",
           get_lan_status(mon) ? "UP" : "DOWN",
           get_last_check_time(mon),
           get_last_error(mon));

    // Step 6: Cleanup
    printf("\n--- Destroying NetworkMonitor ---\n");
    network_monitor_destroy(mon);
    printf("NetworkMonitor destroyed successfully.\n");

    return 0;
}